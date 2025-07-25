// Copyright (c) 2020-2021 The Bitcoin Deis developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/hkdf_sha256_32.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>

#include <cstdint>
#include <string>
#include <vector>

FUZZ_TARGET(crypto_hkdf_hmac_sha256_l32)
{
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};

    const std::vector<uint8_t> initial_key_material = ConsumeRandomLengthByteVector(fuzzed_data_provider);

    CHKDF_HMAC_SHA256_L32 hkdf_hmac_sha256_l32(initial_key_material.data(), initial_key_material.size(), fuzzed_data_provider.ConsumeRandomLengthString(1024));
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 10000) {
        std::vector<uint8_t> out(32);
        hkdf_hmac_sha256_l32.Expand32(fuzzed_data_provider.ConsumeRandomLengthString(128), out.data());
    }
}
