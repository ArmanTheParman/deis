// Copyright (c) 2020 The Bitcoin Deis developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

bool CastToBool(const std::vector<unsigned char>& vch);

FUZZ_TARGET(script_interpreter)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    {
        const CScript script_code = ConsumeScript(fuzzed_data_provider);
        const std::optional<CMutableTransaction> mtx = ConsumeDeserializable<CMutableTransaction>(fuzzed_data_provider, TX_WITH_WITNESS);
        if (mtx) {
            const CTransaction tx_to{*mtx};
            const unsigned int in = fuzzed_data_provider.ConsumeIntegral<unsigned int>();
            if (in < tx_to.vin.size()) {
                (void)SignatureHash(script_code, tx_to, in, fuzzed_data_provider.ConsumeIntegral<int>(), ConsumeMoney(fuzzed_data_provider), fuzzed_data_provider.PickValueInArray({SigVersion::BASE, SigVersion::WITNESS_V0}), nullptr);
                const std::optional<CMutableTransaction> mtx_precomputed = ConsumeDeserializable<CMutableTransaction>(fuzzed_data_provider, TX_WITH_WITNESS);
                if (mtx_precomputed) {
                    const CTransaction tx_precomputed{*mtx_precomputed};
                    const PrecomputedTransactionData precomputed_transaction_data{tx_precomputed};
                    (void)SignatureHash(script_code, tx_to, in, fuzzed_data_provider.ConsumeIntegral<int>(), ConsumeMoney(fuzzed_data_provider), fuzzed_data_provider.PickValueInArray({SigVersion::BASE, SigVersion::WITNESS_V0}), &precomputed_transaction_data);
                }
            }
        }
    }
    {
        (void)CastToBool(ConsumeRandomLengthByteVector(fuzzed_data_provider));
    }
}
