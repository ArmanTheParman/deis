// Copyright (c) 2020-2021 The Bitcoin Deis developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/bloom.h>
#include <primitives/transaction.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>

#include <cassert>
#include <limits>
#include <optional>
#include <vector>

FUZZ_TARGET(bloom_filter)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    bool good_data{true};

    CBloomFilter bloom_filter{
        fuzzed_data_provider.ConsumeIntegralInRange<unsigned int>(1, 10000000),
        1.0 / fuzzed_data_provider.ConsumeIntegralInRange<unsigned int>(1, std::numeric_limits<unsigned int>::max()),
        fuzzed_data_provider.ConsumeIntegral<unsigned int>(),
        static_cast<unsigned char>(fuzzed_data_provider.PickValueInArray({BLOOM_UPDATE_NONE, BLOOM_UPDATE_ALL, BLOOM_UPDATE_P2PUBKEY_ONLY, BLOOM_UPDATE_MASK}))};
    LIMITED_WHILE(good_data && fuzzed_data_provider.remaining_bytes() > 0, 10'000)
    {
        CallOneOf(
            fuzzed_data_provider,
            [&] {
                const std::vector<unsigned char> b = ConsumeRandomLengthByteVector(fuzzed_data_provider);
                (void)bloom_filter.contains(b);
                bloom_filter.insert(b);
                const bool present = bloom_filter.contains(b);
                assert(present);
            },
            [&] {
                const std::optional<COutPoint> out_point = ConsumeDeserializable<COutPoint>(fuzzed_data_provider);
                if (!out_point) {
                    good_data = false;
                    return;
                }
                (void)bloom_filter.contains(*out_point);
                bloom_filter.insert(*out_point);
                const bool present = bloom_filter.contains(*out_point);
                assert(present);
            },
            [&] {
                const std::optional<uint256> u256 = ConsumeDeserializable<uint256>(fuzzed_data_provider);
                if (!u256) {
                    good_data = false;
                    return;
                }
                (void)bloom_filter.contains(*u256);
                bloom_filter.insert(*u256);
                const bool present = bloom_filter.contains(*u256);
                assert(present);
            },
            [&] {
                const std::optional<CMutableTransaction> mut_tx = ConsumeDeserializable<CMutableTransaction>(fuzzed_data_provider, TX_WITH_WITNESS);
                if (!mut_tx) {
                    good_data = false;
                    return;
                }
                const CTransaction tx{*mut_tx};
                (void)bloom_filter.IsRelevantAndUpdate(tx);
            });
        (void)bloom_filter.IsWithinSizeConstraints();
    }
}
