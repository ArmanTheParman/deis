// Copyright (c) 2023-present The Bitcoin Deis developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <config/bitcoin-config.h> // IWYU pragma: keep

#include <bench/bench.h>
#include <node/context.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <wallet/context.h>
#include <wallet/wallet.h>

namespace wallet {
static void WalletCreate(benchmark::Bench& bench, bool encrypted)
{
    auto test_setup = MakeNoLogFileContext<TestingSetup>();
    FastRandomContext random;

    WalletContext context;
    context.args = &test_setup->m_args;
    context.chain = test_setup->m_node.chain.get();

    DatabaseOptions options;
    options.require_format = DatabaseFormat::SQLITE;
    options.require_create = true;
    options.create_flags = WALLET_FLAG_DESCRIPTORS;

    if (encrypted) {
        options.create_passphrase = random.rand256().ToString();
    }

    DatabaseStatus status;
    bilingual_str error_string;
    std::vector<bilingual_str> warnings;

    auto wallet_path = fs::PathToString(test_setup->m_path_root / "test_wallet");
    bench.run([&] {
        auto wallet = CreateWallet(context, wallet_path, /*load_on_start=*/std::nullopt, options, status, error_string, warnings);
        assert(status == DatabaseStatus::SUCCESS);
        assert(wallet != nullptr);

        // Release wallet
        RemoveWallet(context, wallet, /*load_on_start=*/ std::nullopt);
        WaitForDeleteWallet(std::move(wallet));
        fs::remove_all(wallet_path);
    });
}

static void WalletCreatePlain(benchmark::Bench& bench) { WalletCreate(bench, /*encrypted=*/false); }
static void WalletCreateEncrypted(benchmark::Bench& bench) { WalletCreate(bench, /*encrypted=*/true); }

#ifdef USE_SQLITE
BENCHMARK(WalletCreatePlain, benchmark::PriorityLevel::LOW);
BENCHMARK(WalletCreateEncrypted, benchmark::PriorityLevel::LOW);
#endif

} // namespace wallet
