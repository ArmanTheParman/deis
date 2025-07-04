// Copyright (c) The Bitcoin Deis developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cluster_linearize.h>
#include <serialize.h>
#include <streams.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/util/cluster_linearize.h>
#include <util/bitset.h>
#include <util/feefrac.h>

#include <algorithm>
#include <stdint.h>
#include <vector>
#include <utility>

using namespace cluster_linearize;

namespace {

/** A simple finder class for candidate sets.
 *
 * This class matches SearchCandidateFinder in interface and behavior, though with fewer
 * optimizations.
 */
template<typename SetType>
class SimpleCandidateFinder
{
    /** Internal dependency graph. */
    const DepGraph<SetType>& m_depgraph;
    /** Which transaction are left to include. */
    SetType m_todo;

public:
    /** Construct an SimpleCandidateFinder for a given graph. */
    SimpleCandidateFinder(const DepGraph<SetType>& depgraph LIFETIMEBOUND) noexcept :
        m_depgraph(depgraph), m_todo{SetType::Fill(depgraph.TxCount())} {}

    /** Remove a set of transactions from the set of to-be-linearized ones. */
    void MarkDone(SetType select) noexcept { m_todo -= select; }

    /** Determine whether unlinearized transactions remain. */
    bool AllDone() const noexcept { return m_todo.None(); }

    /** Find a candidate set using at most max_iterations iterations, and the number of iterations
     *  actually performed. If that number is less than max_iterations, then the result is optimal.
     *
     * Complexity: O(N * M), where M is the number of connected topological subsets of the cluster.
     *             That number is bounded by M <= 2^(N-1).
     */
    std::pair<SetInfo<SetType>, uint64_t> FindCandidateSet(uint64_t max_iterations) const noexcept
    {
        uint64_t iterations_left = max_iterations;
        // Queue of work units. Each consists of:
        // - inc: set of transactions definitely included
        // - und: set of transactions that can be added to inc still
        std::vector<std::pair<SetType, SetType>> queue;
        // Initially we have just one queue element, with the entire graph in und.
        queue.emplace_back(SetType{}, m_todo);
        // Best solution so far.
        SetInfo best(m_depgraph, m_todo);
        // Process the queue.
        while (!queue.empty() && iterations_left) {
            --iterations_left;
            // Pop top element of the queue.
            auto [inc, und] = queue.back();
            queue.pop_back();
            // Look for a transaction to consider adding/removing.
            bool inc_none = inc.None();
            for (auto split : und) {
                // If inc is empty, consider any split transaction. Otherwise only consider
                // transactions that share ancestry with inc so far (which means only connected
                // sets will be considered).
                if (inc_none || inc.Overlaps(m_depgraph.Ancestors(split))) {
                    // Add a queue entry with split included.
                    SetInfo new_inc(m_depgraph, inc | (m_todo & m_depgraph.Ancestors(split)));
                    queue.emplace_back(new_inc.transactions, und - new_inc.transactions);
                    // Add a queue entry with split excluded.
                    queue.emplace_back(inc, und - m_depgraph.Descendants(split));
                    // Update statistics to account for the candidate new_inc.
                    if (new_inc.feerate > best.feerate) best = new_inc;
                    break;
                }
            }
        }
        return {std::move(best), max_iterations - iterations_left};
    }
};

/** A very simple finder class for optimal candidate sets, which tries every subset.
 *
 * It is even simpler than SimpleCandidateFinder, and is primarily included here to test the
 * correctness of SimpleCandidateFinder, which is then used to test the correctness of
 * SearchCandidateFinder.
 */
template<typename SetType>
class ExhaustiveCandidateFinder
{
    /** Internal dependency graph. */
    const DepGraph<SetType>& m_depgraph;
    /** Which transaction are left to include. */
    SetType m_todo;

public:
    /** Construct an ExhaustiveCandidateFinder for a given graph. */
    ExhaustiveCandidateFinder(const DepGraph<SetType>& depgraph LIFETIMEBOUND) noexcept :
        m_depgraph(depgraph), m_todo{SetType::Fill(depgraph.TxCount())} {}

    /** Remove a set of transactions from the set of to-be-linearized ones. */
    void MarkDone(SetType select) noexcept { m_todo -= select; }

    /** Determine whether unlinearized transactions remain. */
    bool AllDone() const noexcept { return m_todo.None(); }

    /** Find the optimal remaining candidate set.
     *
     * Complexity: O(N * 2^N).
     */
    SetInfo<SetType> FindCandidateSet() const noexcept
    {
        // Best solution so far.
        SetInfo<SetType> best{m_todo, m_depgraph.FeeRate(m_todo)};
        // The number of combinations to try.
        uint64_t limit = (uint64_t{1} << m_todo.Count()) - 1;
        // Try the transitive closure of every non-empty subset of m_todo.
        for (uint64_t x = 1; x < limit; ++x) {
            // If bit number b is set in x, then the remaining ancestors of the b'th remaining
            // transaction in m_todo are included.
            SetType txn;
            auto x_shifted{x};
            for (auto i : m_todo) {
                if (x_shifted & 1) txn |= m_depgraph.Ancestors(i);
                x_shifted >>= 1;
            }
            SetInfo cur(m_depgraph, txn & m_todo);
            if (cur.feerate > best.feerate) best = cur;
        }
        return best;
    }
};

/** A simple linearization algorithm.
 *
 * This matches Linearize() in interface and behavior, though with fewer optimizations, lacking
 * the ability to pass in an existing linearization, and using just SimpleCandidateFinder rather
 * than AncestorCandidateFinder and SearchCandidateFinder.
 */
template<typename SetType>
std::pair<std::vector<ClusterIndex>, bool> SimpleLinearize(const DepGraph<SetType>& depgraph, uint64_t max_iterations)
{
    std::vector<ClusterIndex> linearization;
    SimpleCandidateFinder finder(depgraph);
    SetType todo = SetType::Fill(depgraph.TxCount());
    bool optimal = true;
    while (todo.Any()) {
        auto [candidate, iterations_done] = finder.FindCandidateSet(max_iterations);
        if (iterations_done == max_iterations) optimal = false;
        depgraph.AppendTopo(linearization, candidate.transactions);
        todo -= candidate.transactions;
        finder.MarkDone(candidate.transactions);
        max_iterations -= iterations_done;
    }
    return {std::move(linearization), optimal};
}

/** Given a dependency graph, and a todo set, read a topological subset of todo from reader. */
template<typename SetType>
SetType ReadTopologicalSet(const DepGraph<SetType>& depgraph, const SetType& todo, SpanReader& reader)
{
    uint64_t mask{0};
    try {
        reader >> VARINT(mask);
    } catch(const std::ios_base::failure&) {}
    SetType ret;
    for (auto i : todo) {
        if (!ret[i]) {
            if (mask & 1) ret |= depgraph.Ancestors(i);
            mask >>= 1;
        }
    }
    return ret & todo;
}

/** Given a dependency graph, construct any valid linearization for it, reading from a SpanReader. */
template<typename BS>
std::vector<ClusterIndex> ReadLinearization(const DepGraph<BS>& depgraph, SpanReader& reader)
{
    std::vector<ClusterIndex> linearization;
    TestBitSet todo = TestBitSet::Fill(depgraph.TxCount());
    // In every iteration one topologically-valid transaction is appended to linearization.
    while (todo.Any()) {
        // Compute the set of transactions with no not-yet-included ancestors.
        TestBitSet potential_next;
        for (auto j : todo) {
            if ((depgraph.Ancestors(j) & todo) == TestBitSet::Singleton(j)) {
                potential_next.Set(j);
            }
        }
        // There must always be one (otherwise there is a cycle in the graph).
        assert(potential_next.Any());
        // Read a number from reader, and interpret it as index into potential_next.
        uint64_t idx{0};
        try {
            reader >> VARINT(idx);
        } catch (const std::ios_base::failure&) {}
        idx %= potential_next.Count();
        // Find out which transaction that corresponds to.
        for (auto j : potential_next) {
            if (idx == 0) {
                // When found, add it to linearization and remove it from todo.
                linearization.push_back(j);
                assert(todo[j]);
                todo.Reset(j);
                break;
            }
            --idx;
        }
    }
    return linearization;
}

} // namespace

FUZZ_TARGET(clusterlin_add_dependency)
{
    // Verify that computing a DepGraph from a cluster, or building it step by step using AddDependency
    // have the same effect.

    // Construct a cluster of a certain length, with no dependencies.
    FuzzedDataProvider provider(buffer.data(), buffer.size());
    auto num_tx = provider.ConsumeIntegralInRange<ClusterIndex>(2, 32);
    Cluster<TestBitSet> cluster(num_tx, std::pair{FeeFrac{0, 1}, TestBitSet{}});
    // Construct the corresponding DepGraph object (also no dependencies).
    DepGraph depgraph(cluster);
    SanityCheck(depgraph);
    // Read (parent, child) pairs, and add them to the cluster and depgraph.
    LIMITED_WHILE(provider.remaining_bytes() > 0, TestBitSet::Size() * TestBitSet::Size()) {
        auto parent = provider.ConsumeIntegralInRange<ClusterIndex>(0, num_tx - 1);
        auto child = provider.ConsumeIntegralInRange<ClusterIndex>(0, num_tx - 2);
        child += (child >= parent);
        cluster[child].second.Set(parent);
        depgraph.AddDependency(parent, child);
        assert(depgraph.Ancestors(child)[parent]);
        assert(depgraph.Descendants(parent)[child]);
    }
    // Sanity check the result.
    SanityCheck(depgraph);
    // Verify that the resulting DepGraph matches one recomputed from the cluster.
    assert(DepGraph(cluster) == depgraph);
}

FUZZ_TARGET(clusterlin_cluster_serialization)
{
    // Verify that any graph of transactions has its ancestry correctly computed by DepGraph, and
    // if it is a DAG, that it can be serialized as a DepGraph in a way that roundtrips. This
    // guarantees that any acyclic cluster has a corresponding DepGraph serialization.

    FuzzedDataProvider provider(buffer.data(), buffer.size());

    // Construct a cluster in a naive way (using a FuzzedDataProvider-based serialization).
    Cluster<TestBitSet> cluster;
    auto num_tx = provider.ConsumeIntegralInRange<ClusterIndex>(1, 32);
    cluster.resize(num_tx);
    for (ClusterIndex i = 0; i < num_tx; ++i) {
        cluster[i].first.size = provider.ConsumeIntegralInRange<int32_t>(1, 0x3fffff);
        cluster[i].first.fee = provider.ConsumeIntegralInRange<int64_t>(-0x8000000000000, 0x7ffffffffffff);
        for (ClusterIndex j = 0; j < num_tx; ++j) {
            if (i == j) continue;
            if (provider.ConsumeBool()) cluster[i].second.Set(j);
        }
    }

    // Construct dependency graph, and verify it matches the cluster (which includes a round-trip
    // check for the serialization).
    DepGraph depgraph(cluster);
    VerifyDepGraphFromCluster(cluster, depgraph);
}

FUZZ_TARGET(clusterlin_depgraph_serialization)
{
    // Verify that any deserialized depgraph is acyclic and roundtrips to an identical depgraph.

    // Construct a graph by deserializing.
    SpanReader reader(buffer);
    DepGraph<TestBitSet> depgraph;
    try {
        reader >> Using<DepGraphFormatter>(depgraph);
    } catch (const std::ios_base::failure&) {}
    SanityCheck(depgraph);

    // Verify the graph is a DAG.
    assert(IsAcyclic(depgraph));
}

FUZZ_TARGET(clusterlin_components)
{
    // Verify the behavior of DepGraphs's FindConnectedComponent and IsConnected functions.

    // Construct a depgraph.
    SpanReader reader(buffer);
    DepGraph<TestBitSet> depgraph;
    try {
        reader >> Using<DepGraphFormatter>(depgraph);
    } catch (const std::ios_base::failure&) {}

    TestBitSet todo = TestBitSet::Fill(depgraph.TxCount());
    while (todo.Any()) {
        // Find a connected component inside todo.
        auto component = depgraph.FindConnectedComponent(todo);

        // The component must be a subset of todo and non-empty.
        assert(component.IsSubsetOf(todo));
        assert(component.Any());

        // If todo is the entire graph, and the entire graph is connected, then the component must
        // be the entire graph.
        if (todo == TestBitSet::Fill(depgraph.TxCount())) {
            assert((component == todo) == depgraph.IsConnected());
        }

        // If subset is connected, then component must match subset.
        assert((component == todo) == depgraph.IsConnected(todo));

        // The component cannot have any ancestors or descendants outside of component but in todo.
        for (auto i : component) {
            assert((depgraph.Ancestors(i) & todo).IsSubsetOf(component));
            assert((depgraph.Descendants(i) & todo).IsSubsetOf(component));
        }

        // Starting from any component element, we must be able to reach every element.
        for (auto i : component) {
            // Start with just i as reachable.
            TestBitSet reachable = TestBitSet::Singleton(i);
            // Add in-todo descendants and ancestors to reachable until it does not change anymore.
            while (true) {
                TestBitSet new_reachable = reachable;
                for (auto j : new_reachable) {
                    new_reachable |= depgraph.Ancestors(j) & todo;
                    new_reachable |= depgraph.Descendants(j) & todo;
                }
                if (new_reachable == reachable) break;
                reachable = new_reachable;
            }
            // Verify that the result is the entire component.
            assert(component == reachable);
        }

        // Construct an arbitrary subset of todo.
        uint64_t subset_bits{0};
        try {
            reader >> VARINT(subset_bits);
        } catch (const std::ios_base::failure&) {}
        TestBitSet subset;
        for (ClusterIndex i = 0; i < depgraph.TxCount(); ++i) {
            if (todo[i]) {
                if (subset_bits & 1) subset.Set(i);
                subset_bits >>= 1;
            }
        }
        // Which must be non-empty.
        if (subset.None()) subset = TestBitSet::Singleton(todo.First());
        // Remove it from todo.
        todo -= subset;
    }

    // No components can be found in an empty subset.
    assert(depgraph.FindConnectedComponent(todo).None());
}

FUZZ_TARGET(clusterlin_chunking)
{
    // Verify the correctness of the ChunkLinearization function.

    // Construct a graph by deserializing.
    SpanReader reader(buffer);
    DepGraph<TestBitSet> depgraph;
    try {
        reader >> Using<DepGraphFormatter>(depgraph);
    } catch (const std::ios_base::failure&) {}

    // Read a valid linearization for depgraph.
    auto linearization = ReadLinearization(depgraph, reader);

    // Invoke the chunking function.
    auto chunking = ChunkLinearization(depgraph, linearization);

    // Verify that chunk feerates are monotonically non-increasing.
    for (size_t i = 1; i < chunking.size(); ++i) {
        assert(!(chunking[i] >> chunking[i - 1]));
    }

    // Naively recompute the chunks (each is the highest-feerate prefix of what remains).
    auto todo = TestBitSet::Fill(depgraph.TxCount());
    for (const auto& chunk_feerate : chunking) {
        assert(todo.Any());
        SetInfo<TestBitSet> accumulator, best;
        for (ClusterIndex idx : linearization) {
            if (todo[idx]) {
                accumulator |= SetInfo(depgraph, idx);
                if (best.feerate.IsEmpty() || accumulator.feerate >> best.feerate) {
                    best = accumulator;
                }
            }
        }
        assert(chunk_feerate == best.feerate);
        assert(best.transactions.IsSubsetOf(todo));
        todo -= best.transactions;
    }
    assert(todo.None());
}

FUZZ_TARGET(clusterlin_ancestor_finder)
{
    // Verify that AncestorCandidateFinder works as expected.

    // Retrieve a depgraph from the fuzz input.
    SpanReader reader(buffer);
    DepGraph<TestBitSet> depgraph;
    try {
        reader >> Using<DepGraphFormatter>(depgraph);
    } catch (const std::ios_base::failure&) {}

    AncestorCandidateFinder anc_finder(depgraph);
    auto todo = TestBitSet::Fill(depgraph.TxCount());
    while (todo.Any()) {
        // Call the ancestor finder's FindCandidateSet for what remains of the graph.
        assert(!anc_finder.AllDone());
        auto best_anc = anc_finder.FindCandidateSet();
        // Sanity check the result.
        assert(best_anc.transactions.Any());
        assert(best_anc.transactions.IsSubsetOf(todo));
        assert(depgraph.FeeRate(best_anc.transactions) == best_anc.feerate);
        assert(depgraph.IsConnected(best_anc.transactions));
        // Check that it is topologically valid.
        for (auto i : best_anc.transactions) {
            assert((depgraph.Ancestors(i) & todo).IsSubsetOf(best_anc.transactions));
        }

        // Compute all remaining ancestor sets.
        std::optional<SetInfo<TestBitSet>> real_best_anc;
        for (auto i : todo) {
            SetInfo info(depgraph, todo & depgraph.Ancestors(i));
            if (!real_best_anc.has_value() || info.feerate > real_best_anc->feerate) {
                real_best_anc = info;
            }
        }
        // The set returned by anc_finder must equal the real best ancestor sets.
        assert(real_best_anc.has_value());
        assert(*real_best_anc == best_anc);

        // Find a topologically valid subset of transactions to remove from the graph.
        auto del_set = ReadTopologicalSet(depgraph, todo, reader);
        // If we did not find anything, use best_anc itself, because we should remove something.
        if (del_set.None()) del_set = best_anc.transactions;
        todo -= del_set;
        anc_finder.MarkDone(del_set);
    }
    assert(anc_finder.AllDone());
}

static constexpr auto MAX_SIMPLE_ITERATIONS = 300000;

FUZZ_TARGET(clusterlin_search_finder)
{
    // Verify that SearchCandidateFinder works as expected by sanity checking the results
    // and comparing with the results from SimpleCandidateFinder, ExhaustiveCandidateFinder, and
    // AncestorCandidateFinder.

    // Retrieve an RNG seed and a depgraph from the fuzz input.
    SpanReader reader(buffer);
    DepGraph<TestBitSet> depgraph;
    uint64_t rng_seed{0};
    try {
        reader >> Using<DepGraphFormatter>(depgraph) >> rng_seed;
    } catch (const std::ios_base::failure&) {}

    // Instantiate ALL the candidate finders.
    SearchCandidateFinder src_finder(depgraph, rng_seed);
    SimpleCandidateFinder smp_finder(depgraph);
    ExhaustiveCandidateFinder exh_finder(depgraph);
    AncestorCandidateFinder anc_finder(depgraph);

    auto todo = TestBitSet::Fill(depgraph.TxCount());
    while (todo.Any()) {
        assert(!src_finder.AllDone());
        assert(!smp_finder.AllDone());
        assert(!exh_finder.AllDone());
        assert(!anc_finder.AllDone());

        // For each iteration, read an iteration count limit from the fuzz input.
        uint64_t max_iterations = 1;
        try {
            reader >> VARINT(max_iterations);
        } catch (const std::ios_base::failure&) {}
        max_iterations &= 0xfffff;

        // Read an initial subset from the fuzz input.
        SetInfo init_best(depgraph, ReadTopologicalSet(depgraph, todo, reader));

        // Call the search finder's FindCandidateSet for what remains of the graph.
        auto [found, iterations_done] = src_finder.FindCandidateSet(max_iterations, init_best);

        // Sanity check the result.
        assert(iterations_done <= max_iterations);
        assert(found.transactions.Any());
        assert(found.transactions.IsSubsetOf(todo));
        assert(depgraph.FeeRate(found.transactions) == found.feerate);
        if (!init_best.feerate.IsEmpty()) assert(found.feerate >= init_best.feerate);
        // Check that it is topologically valid.
        for (auto i : found.transactions) {
            assert(found.transactions.IsSupersetOf(depgraph.Ancestors(i) & todo));
        }

        // At most 2^N-1 iterations can be required: the number of non-empty subsets a graph with N
        // transactions has.
        assert(iterations_done <= ((uint64_t{1} << todo.Count()) - 1));

        // Perform quality checks only if SearchCandidateFinder claims an optimal result.
        if (iterations_done < max_iterations) {
            // Optimal sets are always connected.
            assert(depgraph.IsConnected(found.transactions));

            // Compare with SimpleCandidateFinder.
            auto [simple, simple_iters] = smp_finder.FindCandidateSet(MAX_SIMPLE_ITERATIONS);
            assert(found.feerate >= simple.feerate);
            if (simple_iters < MAX_SIMPLE_ITERATIONS) {
                assert(found.feerate == simple.feerate);
            }

            // Compare with AncestorCandidateFinder;
            auto anc = anc_finder.FindCandidateSet();
            assert(found.feerate >= anc.feerate);

            // Compare with ExhaustiveCandidateFinder. This quickly gets computationally expensive
            // for large clusters (O(2^n)), so only do it for sufficiently small ones.
            if (todo.Count() <= 12) {
                auto exhaustive = exh_finder.FindCandidateSet();
                assert(exhaustive.feerate == found.feerate);
                // Also compare ExhaustiveCandidateFinder with SimpleCandidateFinder (this is
                // primarily a test for SimpleCandidateFinder's correctness).
                assert(exhaustive.feerate >= simple.feerate);
                if (simple_iters < MAX_SIMPLE_ITERATIONS) {
                    assert(exhaustive.feerate == simple.feerate);
                }
            }
        }

        // Find a topologically valid subset of transactions to remove from the graph.
        auto del_set = ReadTopologicalSet(depgraph, todo, reader);
        // If we did not find anything, use found itself, because we should remove something.
        if (del_set.None()) del_set = found.transactions;
        todo -= del_set;
        src_finder.MarkDone(del_set);
        smp_finder.MarkDone(del_set);
        exh_finder.MarkDone(del_set);
        anc_finder.MarkDone(del_set);
    }

    assert(src_finder.AllDone());
    assert(smp_finder.AllDone());
    assert(exh_finder.AllDone());
    assert(anc_finder.AllDone());
}

FUZZ_TARGET(clusterlin_linearization_chunking)
{
    // Verify the behavior of LinearizationChunking.

    // Retrieve a depgraph from the fuzz input.
    SpanReader reader(buffer);
    DepGraph<TestBitSet> depgraph;
    try {
        reader >> Using<DepGraphFormatter>(depgraph);
    } catch (const std::ios_base::failure&) {}

    // Retrieve a topologically-valid subset of depgraph.
    auto todo = TestBitSet::Fill(depgraph.TxCount());
    auto subset = SetInfo(depgraph, ReadTopologicalSet(depgraph, todo, reader));

    // Retrieve a valid linearization for depgraph.
    auto linearization = ReadLinearization(depgraph, reader);

    // Construct a LinearizationChunking object, initially for the whole linearization.
    LinearizationChunking chunking(depgraph, linearization);

    // Incrementally remove transactions from the chunking object, and check various properties at
    // every step.
    while (todo.Any()) {
        assert(chunking.NumChunksLeft() > 0);

        // Construct linearization with just todo.
        std::vector<ClusterIndex> linearization_left;
        for (auto i : linearization) {
            if (todo[i]) linearization_left.push_back(i);
        }

        // Compute the chunking for linearization_left.
        auto chunking_left = ChunkLinearization(depgraph, linearization_left);

        // Verify that it matches the feerates of the chunks of chunking.
        assert(chunking.NumChunksLeft() == chunking_left.size());
        for (ClusterIndex i = 0; i < chunking.NumChunksLeft(); ++i) {
            assert(chunking.GetChunk(i).feerate == chunking_left[i]);
        }

        // Check consistency of chunking.
        TestBitSet combined;
        for (ClusterIndex i = 0; i < chunking.NumChunksLeft(); ++i) {
            const auto& chunk_info = chunking.GetChunk(i);
            // Chunks must be non-empty.
            assert(chunk_info.transactions.Any());
            // Chunk feerates must be monotonically non-increasing.
            if (i > 0) assert(!(chunk_info.feerate >> chunking.GetChunk(i - 1).feerate));
            // Chunks must be a subset of what is left of the linearization.
            assert(chunk_info.transactions.IsSubsetOf(todo));
            // Chunks' claimed feerates must match their transactions' aggregate feerate.
            assert(depgraph.FeeRate(chunk_info.transactions) == chunk_info.feerate);
            // Chunks must be the highest-feerate remaining prefix.
            SetInfo<TestBitSet> accumulator, best;
            for (auto j : linearization) {
                if (todo[j] && !combined[j]) {
                    accumulator |= SetInfo(depgraph, j);
                    if (best.feerate.IsEmpty() || accumulator.feerate > best.feerate) {
                        best = accumulator;
                    }
                }
            }
            assert(best.transactions == chunk_info.transactions);
            assert(best.feerate == chunk_info.feerate);
            // Chunks cannot overlap.
            assert(!chunk_info.transactions.Overlaps(combined));
            combined |= chunk_info.transactions;
            // Chunks must be topological.
            for (auto idx : chunk_info.transactions) {
                assert((depgraph.Ancestors(idx) & todo).IsSubsetOf(combined));
            }
        }
        assert(combined == todo);

        // Verify the expected properties of LinearizationChunking::IntersectPrefixes:
        auto intersect = chunking.IntersectPrefixes(subset);
        // - Intersecting again doesn't change the result.
        assert(chunking.IntersectPrefixes(intersect) == intersect);
        // - The intersection is topological.
        TestBitSet intersect_anc;
        for (auto idx : intersect.transactions) {
            intersect_anc |= (depgraph.Ancestors(idx) & todo);
        }
        assert(intersect.transactions == intersect_anc);
        // - The claimed intersection feerate matches its transactions.
        assert(intersect.feerate == depgraph.FeeRate(intersect.transactions));
        // - The intersection may only be empty if its input is empty.
        assert(intersect.transactions.Any() == subset.transactions.Any());
        // - The intersection feerate must be as high as the input.
        assert(intersect.feerate >= subset.feerate);
        // - No non-empty intersection between the intersection and a prefix of the chunks of the
        //   remainder of the linearization may be better than the intersection.
        TestBitSet prefix;
        for (ClusterIndex i = 0; i < chunking.NumChunksLeft(); ++i) {
            prefix |= chunking.GetChunk(i).transactions;
            auto reintersect = SetInfo(depgraph, prefix & intersect.transactions);
            if (!reintersect.feerate.IsEmpty()) {
                assert(reintersect.feerate <= intersect.feerate);
            }
        }

        // Find a subset to remove from linearization.
        auto done = ReadTopologicalSet(depgraph, todo, reader);
        if (done.None()) {
            // We need to remove a non-empty subset, so fall back to the unlinearized ancestors of
            // the first transaction in todo if done is empty.
            done = depgraph.Ancestors(todo.First()) & todo;
        }
        todo -= done;
        chunking.MarkDone(done);
        subset = SetInfo(depgraph, subset.transactions - done);
    }

    assert(chunking.NumChunksLeft() == 0);
}

FUZZ_TARGET(clusterlin_linearize)
{
    // Verify the behavior of Linearize().

    // Retrieve an RNG seed, an iteration count, and a depgraph from the fuzz input.
    SpanReader reader(buffer);
    DepGraph<TestBitSet> depgraph;
    uint64_t rng_seed{0};
    uint64_t iter_count{0};
    try {
        reader >> VARINT(iter_count) >> Using<DepGraphFormatter>(depgraph) >> rng_seed;
    } catch (const std::ios_base::failure&) {}

    // Optionally construct an old linearization for it.
    std::vector<ClusterIndex> old_linearization;
    {
        uint8_t have_old_linearization{0};
        try {
            reader >> have_old_linearization;
        } catch(const std::ios_base::failure&) {}
        if (have_old_linearization & 1) {
            old_linearization = ReadLinearization(depgraph, reader);
            SanityCheck(depgraph, old_linearization);
        }
    }

    // Invoke Linearize().
    iter_count &= 0x7ffff;
    auto [linearization, optimal] = Linearize(depgraph, iter_count, rng_seed, old_linearization);
    SanityCheck(depgraph, linearization);
    auto chunking = ChunkLinearization(depgraph, linearization);

    // Linearization must always be as good as the old one, if provided.
    if (!old_linearization.empty()) {
        auto old_chunking = ChunkLinearization(depgraph, old_linearization);
        auto cmp = CompareChunks(chunking, old_chunking);
        assert(cmp >= 0);
    }

    // If the iteration count is sufficiently high, an optimal linearization must be found.
    // Each linearization step can use up to 2^k iterations, with steps k=1..n. That sum is
    // 2 * (2^n - 1)
    const uint64_t n = depgraph.TxCount();
    if (n <= 18 && iter_count > 2U * ((uint64_t{1} << n) - 1U)) {
        assert(optimal);
    }

    // If Linearize claims optimal result, run quality tests.
    if (optimal) {
        // It must be as good as SimpleLinearize.
        auto [simple_linearization, simple_optimal] = SimpleLinearize(depgraph, MAX_SIMPLE_ITERATIONS);
        SanityCheck(depgraph, simple_linearization);
        auto simple_chunking = ChunkLinearization(depgraph, simple_linearization);
        auto cmp = CompareChunks(chunking, simple_chunking);
        assert(cmp >= 0);
        // If SimpleLinearize finds the optimal result too, they must be equal (if not,
        // SimpleLinearize is broken).
        if (simple_optimal) assert(cmp == 0);

        // Only for very small clusters, test every topologically-valid permutation.
        if (depgraph.TxCount() <= 7) {
            std::vector<ClusterIndex> perm_linearization(depgraph.TxCount());
            for (ClusterIndex i = 0; i < depgraph.TxCount(); ++i) perm_linearization[i] = i;
            // Iterate over all valid permutations.
            do {
                // Determine whether perm_linearization is topological.
                TestBitSet perm_done;
                bool perm_is_topo{true};
                for (auto i : perm_linearization) {
                    perm_done.Set(i);
                    if (!depgraph.Ancestors(i).IsSubsetOf(perm_done)) {
                        perm_is_topo = false;
                        break;
                    }
                }
                // If so, verify that the obtained linearization is as good as the permutation.
                if (perm_is_topo) {
                    auto perm_chunking = ChunkLinearization(depgraph, perm_linearization);
                    auto cmp = CompareChunks(chunking, perm_chunking);
                    assert(cmp >= 0);
                }
            } while(std::next_permutation(perm_linearization.begin(), perm_linearization.end()));
        }
    }
}

FUZZ_TARGET(clusterlin_postlinearize)
{
    // Verify expected properties of PostLinearize() on arbitrary linearizations.

    // Retrieve a depgraph from the fuzz input.
    SpanReader reader(buffer);
    DepGraph<TestBitSet> depgraph;
    try {
        reader >> Using<DepGraphFormatter>(depgraph);
    } catch (const std::ios_base::failure&) {}

    // Retrieve a linearization from the fuzz input.
    std::vector<ClusterIndex> linearization;
    linearization = ReadLinearization(depgraph, reader);
    SanityCheck(depgraph, linearization);

    // Produce a post-processed version.
    auto post_linearization = linearization;
    PostLinearize(depgraph, post_linearization);
    SanityCheck(depgraph, post_linearization);

    // Compare diagrams: post-linearization cannot worsen anywhere.
    auto chunking = ChunkLinearization(depgraph, linearization);
    auto post_chunking = ChunkLinearization(depgraph, post_linearization);
    auto cmp = CompareChunks(post_chunking, chunking);
    assert(cmp >= 0);

    // Run again, things can keep improving (and never get worse)
    auto post_post_linearization = post_linearization;
    PostLinearize(depgraph, post_post_linearization);
    SanityCheck(depgraph, post_post_linearization);
    auto post_post_chunking = ChunkLinearization(depgraph, post_post_linearization);
    cmp = CompareChunks(post_post_chunking, post_chunking);
    assert(cmp >= 0);

    // The chunks that come out of postlinearizing are always connected.
    LinearizationChunking linchunking(depgraph, post_linearization);
    while (linchunking.NumChunksLeft()) {
        assert(depgraph.IsConnected(linchunking.GetChunk(0).transactions));
        linchunking.MarkDone(linchunking.GetChunk(0).transactions);
    }
}

FUZZ_TARGET(clusterlin_postlinearize_tree)
{
    // Verify expected properties of PostLinearize() on linearizations of graphs that form either
    // an upright or reverse tree structure.

    // Construct a direction, RNG seed, and an arbitrary graph from the fuzz input.
    SpanReader reader(buffer);
    uint64_t rng_seed{0};
    DepGraph<TestBitSet> depgraph_gen;
    uint8_t direction{0};
    try {
        reader >> direction >> rng_seed >> Using<DepGraphFormatter>(depgraph_gen);
    } catch (const std::ios_base::failure&) {}

    // Now construct a new graph, copying the nodes, but leaving only the first parent (even
    // direction) or the first child (odd direction).
    DepGraph<TestBitSet> depgraph_tree;
    for (ClusterIndex i = 0; i < depgraph_gen.TxCount(); ++i) {
        depgraph_tree.AddTransaction(depgraph_gen.FeeRate(i));
    }
    if (direction & 1) {
        for (ClusterIndex i = 0; i < depgraph_gen.TxCount(); ++i) {
            auto children = depgraph_gen.Descendants(i) - TestBitSet::Singleton(i);
            // Remove descendants that are children of other descendants.
            for (auto j : children) {
                if (!children[j]) continue;
                children -= depgraph_gen.Descendants(j);
                children.Set(j);
            }
            if (children.Any()) depgraph_tree.AddDependency(i, children.First());
         }
    } else {
        for (ClusterIndex i = 0; i < depgraph_gen.TxCount(); ++i) {
            auto parents = depgraph_gen.Ancestors(i) - TestBitSet::Singleton(i);
            // Remove ancestors that are parents of other ancestors.
            for (auto j : parents) {
                if (!parents[j]) continue;
                parents -= depgraph_gen.Ancestors(j);
                parents.Set(j);
            }
            if (parents.Any()) depgraph_tree.AddDependency(parents.First(), i);
        }
    }

    // Retrieve a linearization from the fuzz input.
    std::vector<ClusterIndex> linearization;
    linearization = ReadLinearization(depgraph_tree, reader);
    SanityCheck(depgraph_tree, linearization);

    // Produce a postlinearized version.
    auto post_linearization = linearization;
    PostLinearize(depgraph_tree, post_linearization);
    SanityCheck(depgraph_tree, post_linearization);

    // Compare diagrams.
    auto chunking = ChunkLinearization(depgraph_tree, linearization);
    auto post_chunking = ChunkLinearization(depgraph_tree, post_linearization);
    auto cmp = CompareChunks(post_chunking, chunking);
    assert(cmp >= 0);

    // Verify that post-linearizing again does not change the diagram. The result must be identical
    // as post_linearization ought to be optimal already with a tree-structured graph.
    auto post_post_linearization = post_linearization;
    PostLinearize(depgraph_tree, post_linearization);
    SanityCheck(depgraph_tree, post_linearization);
    auto post_post_chunking = ChunkLinearization(depgraph_tree, post_post_linearization);
    auto cmp_post = CompareChunks(post_post_chunking, post_chunking);
    assert(cmp_post == 0);

    // Try to find an even better linearization directly. This must not change the diagram for the
    // same reason.
    auto [opt_linearization, _optimal] = Linearize(depgraph_tree, 100000, rng_seed, post_linearization);
    auto opt_chunking = ChunkLinearization(depgraph_tree, opt_linearization);
    auto cmp_opt = CompareChunks(opt_chunking, post_chunking);
    assert(cmp_opt == 0);
}

FUZZ_TARGET(clusterlin_postlinearize_moved_leaf)
{
    // Verify that taking an existing linearization, and moving a leaf to the back, potentially
    // increasing its fee, and then post-linearizing, results in something as good as the
    // original. This guarantees that in an RBF that replaces a transaction with one of the same
    // size but higher fee, applying the "remove conflicts, append new transaction, postlinearize"
    // process will never worsen linearization quality.

    // Construct an arbitrary graph and a fee from the fuzz input.
    SpanReader reader(buffer);
    DepGraph<TestBitSet> depgraph;
    int32_t fee_inc{0};
    try {
        uint64_t fee_inc_code;
        reader >> Using<DepGraphFormatter>(depgraph) >> VARINT(fee_inc_code);
        fee_inc = fee_inc_code & 0x3ffff;
    } catch (const std::ios_base::failure&) {}
    if (depgraph.TxCount() == 0) return;

    // Retrieve two linearizations from the fuzz input.
    auto lin = ReadLinearization(depgraph, reader);
    auto lin_leaf = ReadLinearization(depgraph, reader);

    // Construct a linearization identical to lin, but with the tail end of lin_leaf moved to the
    // back.
    std::vector<ClusterIndex> lin_moved;
    for (auto i : lin) {
        if (i != lin_leaf.back()) lin_moved.push_back(i);
    }
    lin_moved.push_back(lin_leaf.back());

    // Postlinearize lin_moved.
    PostLinearize(depgraph, lin_moved);
    SanityCheck(depgraph, lin_moved);

    // Compare diagrams (applying the fee delta after computing the old one).
    auto old_chunking = ChunkLinearization(depgraph, lin);
    depgraph.FeeRate(lin_leaf.back()).fee += fee_inc;
    auto new_chunking = ChunkLinearization(depgraph, lin_moved);
    auto cmp = CompareChunks(new_chunking, old_chunking);
    assert(cmp >= 0);
}

FUZZ_TARGET(clusterlin_merge)
{
    // Construct an arbitrary graph from the fuzz input.
    SpanReader reader(buffer);
    DepGraph<TestBitSet> depgraph;
    try {
        reader >> Using<DepGraphFormatter>(depgraph);
    } catch (const std::ios_base::failure&) {}

    // Retrieve two linearizations from the fuzz input.
    auto lin1 = ReadLinearization(depgraph, reader);
    auto lin2 = ReadLinearization(depgraph, reader);

    // Merge the two.
    auto lin_merged = MergeLinearizations(depgraph, lin1, lin2);

    // Compute chunkings and compare.
    auto chunking1 = ChunkLinearization(depgraph, lin1);
    auto chunking2 = ChunkLinearization(depgraph, lin2);
    auto chunking_merged = ChunkLinearization(depgraph, lin_merged);
    auto cmp1 = CompareChunks(chunking_merged, chunking1);
    assert(cmp1 >= 0);
    auto cmp2 = CompareChunks(chunking_merged, chunking2);
    assert(cmp2 >= 0);
}
