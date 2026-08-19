#include "check.hpp"

#include <edgy/config.hpp>
#include <edgy/version.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

int
main()
{
    using namespace edgy;
    using edgy::test::expect;

    {
        auto const v = versionString();
        expect(v.find(kVersionBase) == 0, "version string starts with kVersionBase");
        expect(!v.empty(), "version string is not empty");
        expect(v.find("xrpld") != std::string::npos, "xrpld-linked tests report xrpld flavor");
    }
    {
        Config d;
        expect(d.search == Config::kSearchFull, "default [search] is full");
        expect(d.searchFast == Config::kSearchFast, "default [search-fast] is fast");
        expect(d.searchTimeout.count() == 0, "default [timeout-ms] is none");
        expect(d.fullSnapshot, "default [full-snapshot] is full");
        expect(d.snapshotPage == Config::kSnapshotPageMax,
               "default [snapshot-page] is 2048");
        expect(d.midCloseDelay.count() == 100, "default [update-ms] is 100");
        expect(d.network == NetworkKind::xrpl, "xrpld binary is xrpl");
        expect(!d.xahau(), "xrpld binary is not xahau");
        expect(std::string{d.nativeCurrency()} == "XRP", "xrpld native is XRP");
        expect(std::string{d.nodeSoftware()} == "xrpld", "xrpld node software is xrpld");
    }

    {
        expect(rotateDebugLog("").empty(), "empty debug path does not rotate");
        auto const missing = std::string{"/tmp/edgy-test-rotate-missing.log"};
        std::remove(missing.c_str());
        expect(rotateDebugLog(missing).empty(), "missing debug log does not rotate");

        auto const path = std::string{"/tmp/edgy-test-rotate.log"};
        std::remove(path.c_str());
        {
            std::ofstream out(path);
            out << "previous run\n";
        }
        auto const backup = rotateDebugLog(path);
        expect(!backup.empty(), "non-empty debug log is renamed");
        expect(backup.find(path + ".") == 0, "backup keeps the configured path as prefix");
        expect(!std::ifstream(path).good(), "configured path is free after rotate");
        std::ifstream in(backup);
        std::string line;
        std::getline(in, line);
        expect(line == "previous run", "rotated log keeps the previous run");
        expect(rotateDebugLog(path).empty(), "second rotate with no new file is a no-op");
        std::remove(backup.c_str());

        {
            std::ofstream out(path);
        }
        expect(rotateDebugLog(path).empty(), "empty leftover log is not kept as a backup");
        std::remove(path.c_str());
    }


    {
        auto const path = std::string{"/tmp/edgy-test.cfg"};
        {
            std::ofstream out(path);
            out << "# comment\n"
                << "[node]\nws://10.0.0.2:6006\n\n"
                << "[listen-ws]\n127.0.0.1:7008\n"
                << "[listen-rpc]\n127.0.0.1:7005\n"
                << "[workers]\n32\n"
                << "[net-threads]\n2\n"
                << "[update-ms]\n150\n"
                << "[proxy]\n0\n"
                << "[debug]\n/private/tmp/edgy.log\n"
                << "[path_find]\n"
                << "max_total_lines=1234\n"
                << "max_lines_per_account=56\n";
        }
        auto const cfg = Config::fromFile(path);
        expect(cfg.nodeWs == "ws://10.0.0.2:6006", "cfg [node]");
        expect(cfg.listenWs == "127.0.0.1:7008", "cfg [listen-ws]");
        expect(cfg.listenRpc == "127.0.0.1:7005", "cfg [listen-rpc]");
        expect(cfg.workers == 32, "cfg [workers]");
        expect(cfg.netThreads == 2, "cfg [net-threads]");
        expect(cfg.midCloseDelay.count() == 150, "cfg [update-ms]");
        expect(!cfg.proxyOther, "cfg [proxy] 0");
        expect(cfg.debugLog == "/private/tmp/edgy.log", "cfg [debug]");
        expect(cfg.maxTotalLines == 1234, "cfg [path_find] max_total_lines");
        expect(cfg.maxLinesPerAccount == 56, "cfg [path_find] max_lines_per_account");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-stanzas.cfg"};
        {
            std::ofstream out(path);
            out << "[debug]\n/private/tmp/edgy.log\n"
                << "[listen-ws]\n0.0.0.0:6008\n"
                << "[listen-rpc]\n0.0.0.0:5008\n"
                << "[node]\nws://127.0.0.1:6006\n"
                << "[workers]\n64\n"
                << "[net-threads]\n4\n"
                << "[update-ms]\n100\n"
                << "[proxy]\n1\n"
                << "[max_total_lines]\n1000000\n"
                << "[max_lines_per_account]\n50000\n"
                << "[line_chunk_size]\n64\n"
                << "[cache_reuse_ledgers]\n6\n"
                << "[search]\nfull\n"
                << "[search-fast]\nfull\n"
                << "[timeout-ms]\n0\n"
                << "[full-snapshot]\nfull\n"
                << "[snapshot-page]\n2048\n";
        }
        auto const cfg = Config::fromFile(path);
        expect(cfg.debugLog == "/private/tmp/edgy.log", "stanza [debug]");
        expect(cfg.listenWs == "0.0.0.0:6008", "stanza [listen-ws]");
        expect(cfg.listenRpc == "0.0.0.0:5008", "stanza [listen-rpc]");
        expect(cfg.nodeWs == "ws://127.0.0.1:6006", "stanza [node]");
        expect(cfg.workers == 64, "stanza [workers]");
        expect(cfg.netThreads == 4, "stanza [net-threads]");
        expect(cfg.midCloseDelay.count() == 100, "stanza [update-ms]");
        expect(cfg.proxyOther, "stanza [proxy] 1");
        expect(cfg.maxTotalLines == 1'000'000, "stanza [max_total_lines]");
        expect(cfg.maxLinesPerAccount == 50'000, "stanza [max_lines_per_account]");
        expect(cfg.lineChunkSize == 64, "stanza [line_chunk_size]");
        expect(cfg.cacheReuseLedgers == 6, "stanza [cache_reuse_ledgers]");
        expect(cfg.search == Config::kSearchFull, "stanza [search] full");
        expect(cfg.searchFast == Config::kSearchFull, "stanza [search-fast] full");
        expect(cfg.searchTimeout.count() == 0, "stanza [timeout-ms] 0");
        expect(cfg.fullSnapshot, "stanza [full-snapshot] full");
        expect(cfg.snapshotPage == 2048, "stanza [snapshot-page] 2048");
        expect(cfg.network == NetworkKind::xrpl, "file cannot change compile-time network");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-legacy-network.cfg"};
        {
            std::ofstream out(path);
            out << "[network]\nxahau\n[node]\nws://127.0.0.1:6006\n";
        }
        auto const cfg = Config::fromFile(path);
        expect(cfg.nodeWs == "ws://127.0.0.1:6006", "legacy [network] still loads [node]");
        expect(cfg.network == NetworkKind::xrpl, "legacy [network] is ignored");
        expect(!cfg.xahau(), "legacy [network] does not switch family");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-modes.cfg"};
        {
            std::ofstream out(path);
            out << "[search]\nmid\n"
                << "[search-fast]\nfast\n"
                << "[timeout-ms]\n250\n"
                << "[full-snapshot]\nbooks\n"
                << "[snapshot-page]\n256\n";
        }
        auto const cfg = Config::fromFile(path);
        expect(cfg.search == Config::kSearchMid, "stanza [search] mid");
        expect(cfg.searchFast == Config::kSearchFast, "stanza [search-fast] fast");
        expect(cfg.searchTimeout.count() == 250, "stanza [timeout-ms] 250");
        expect(!cfg.fullSnapshot, "stanza [full-snapshot] books");
        expect(cfg.snapshotPage == 256, "stanza [snapshot-page] 256");
    }

    {
        char arg0[] = "edgy";
        char arg1[] = "--conf";
        auto const path = std::string{"/tmp/edgy-test-override.cfg"};
        {
            std::ofstream out(path);
            out << "[node]\nws://10.0.0.2:6006\n[workers]\n16\n";
        }
        char arg2[128];
        std::snprintf(arg2, sizeof(arg2), "%s", path.c_str());
        char arg3[] = "--node";
        char arg4[] = "ws://127.0.0.1:6006";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, nullptr};
        auto const cfg = Config::fromArgs(5, argv);
        expect(cfg.nodeWs == "ws://127.0.0.1:6006", "CLI --node overrides [node]");
        expect(cfg.workers == 16, "file [workers] kept when not on CLI");
    }

    {
        char arg0[] = "edgy";
        char a1[] = "--search";
        char a2[] = "full";
        char a3[] = "--search-fast";
        char a4[] = "fast";
        char a5[] = "--timeout-ms";
        char a6[] = "100";
        char a7[] = "--full-snapshot";
        char a8[] = "0";
        char a9[] = "--snapshot-page";
        char a10[] = "512";
        char* argv[] = {arg0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, nullptr};
        auto const cfg = Config::fromArgs(11, argv);
        expect(cfg.search == Config::kSearchFull, "CLI --search full");
        expect(cfg.searchFast == Config::kSearchFast, "CLI --search-fast fast");
        expect(cfg.searchTimeout.count() == 100, "CLI --timeout-ms 100");
        expect(!cfg.fullSnapshot, "CLI --full-snapshot 0");
        expect(cfg.snapshotPage == 512, "CLI --snapshot-page 512");
    }

    {
        char arg0[] = "edgy";
        char a1[] = "--xahau";
        char* argv[] = {arg0, a1, nullptr};
        bool threw = false;
        try
        {
            (void)Config::fromArgs(2, argv);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "CLI --xahau is rejected");
    }

    {
        char arg0[] = "edgy";
        char a1[] = "--network";
        char a2[] = "xahau";
        char* argv[] = {arg0, a1, a2, nullptr};
        bool threw = false;
        try
        {
            (void)Config::fromArgs(3, argv);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "CLI --network is rejected");
    }

    {
        char arg0[] = "edgy";
        char a1[] = "--xrpl";
        char* argv[] = {arg0, a1, nullptr};
        bool threw = false;
        try
        {
            (void)Config::fromArgs(2, argv);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "CLI --xrpl is rejected");
    }

    {
        char arg0[] = "edgy";
        char a1[] = "--xrpld";
        char* argv[] = {arg0, a1, nullptr};
        bool threw = false;
        try
        {
            (void)Config::fromArgs(2, argv);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "CLI --xrpld is rejected");
    }

    {
        char const* prev = std::getenv("EDGY_NETWORK");
        std::string saved = prev ? prev : "";
        ::setenv("EDGY_NETWORK", "xahau", 1);
        char arg0[] = "edgy";
        char* argv[] = {arg0, nullptr};
        bool threw = false;
        try
        {
            (void)Config::fromArgs(1, argv);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        if (prev)
            ::setenv("EDGY_NETWORK", saved.c_str(), 1);
        else
            ::unsetenv("EDGY_NETWORK");
        expect(threw, "EDGY_NETWORK is rejected");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-node-type.cfg"};
        {
            std::ofstream out(path);
            out << "[node-type]\nxahaud\n[workers]\n8\n";
        }
        auto const cfg = Config::fromFile(path);
        expect(cfg.workers == 8, "legacy [node-type] still loads other stanzas");
        expect(cfg.network == NetworkKind::xrpl, "legacy [node-type] is ignored");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-clamp.cfg"};
        {
            std::ofstream out(path);
            out << "[workers]\n0\n[net-threads]\n0\n[update-ms]\n1\n"
                << "[search]\nmid\n[search-fast]\nfull\n[timeout-ms]\n-5\n"
                << "[snapshot-page]\n0\n";
        }
        auto const cfg = Config::fromFile(path);
        expect(cfg.workers == 1, "workers 0 clamps to 1");
        expect(cfg.netThreads == 1, "net-threads 0 clamps to 1");
        expect(cfg.midCloseDelay.count() == 20, "update-ms below 20 clamps to 20");
        expect(cfg.search == Config::kSearchMid, "search mid is kept");
        expect(cfg.searchFast == Config::kSearchMid, "search-fast full clamps to search mid");
        expect(cfg.searchTimeout.count() == 0, "negative timeout-ms clamps to 0");
        expect(cfg.snapshotPage == 1, "snapshot-page 0 clamps to 1");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-workers-max.cfg"};
        {
            std::ofstream out(path);
            out << "[workers]\n999\n";
        }
        auto const cfg = Config::fromFile(path);
        expect(cfg.workers == 256, "workers above 256 clamp to 256");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-snapshot-page-max.cfg"};
        {
            std::ofstream out(path);
            out << "[snapshot-page]\n99999\n";
        }
        auto const cfg = Config::fromFile(path);
        expect(cfg.snapshotPage == Config::kSnapshotPageMax,
               "snapshot-page above 2048 clamps to 2048");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-unknown-section.cfg"};
        {
            std::ofstream out(path);
            out << "[not-a-stanza]\n1\n";
        }
        bool threw = false;
        try
        {
            (void)Config::fromFile(path);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "unknown config section throws");
    }

    {
        bool threw = false;
        try
        {
            (void)Config::fromFile("/tmp/edgy-does-not-exist.cfg");
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "missing config file throws");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-bare.cfg"};
        {
            std::ofstream out(path);
            out << "ws://127.0.0.1:1\n";
        }
        bool threw = false;
        try
        {
            (void)Config::fromFile(path);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "value outside a section throws");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-pathfind-badkey.cfg"};
        {
            std::ofstream out(path);
            out << "[path_find]\nnot_a_key=1\n";
        }
        bool threw = false;
        try
        {
            (void)Config::fromFile(path);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "unknown [path_find] key throws");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-search-numeric.cfg"};
        {
            std::ofstream out(path);
            out << "[search]\n2\n";
        }
        bool threw = false;
        try
        {
            (void)Config::fromFile(path);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "numeric [search] is rejected");
    }

    {
        auto const path = std::string{"/tmp/edgy-test-search-xrpld.cfg"};
        {
            std::ofstream out(path);
            out << "[search-fast]\n7\n";
        }
        bool threw = false;
        try
        {
            (void)Config::fromFile(path);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "xrpld path_search numbers are rejected");
    }

    {
        char arg0[] = "edgy";
        char a1[] = "--not-a-flag";
        char* argv[] = {arg0, a1, nullptr};
        bool threw = false;
        try
        {
            (void)Config::fromArgs(2, argv);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        expect(threw, "unknown CLI flag throws");
    }

    {
        struct Shipped
        {
            char const* rel;
            char const* abs;
            char const* listenWs;
            char const* listenRpc;
            char const* debug;
            char const* label;
        };
        Shipped const shipped[] = {
            {"cfg/edgy-xrpl.example.cfg",
             "/Users/fomo/Dev/Ledgers/PathFinder/cfg/edgy-xrpl.example.cfg",
             "0.0.0.0:6008",
             "0.0.0.0:5008",
             "/tmp/edgy-xrpl.log",
             "xrpl"},
            {"cfg/edgy-xahau.example.cfg",
             "/Users/fomo/Dev/Ledgers/PathFinder/cfg/edgy-xahau.example.cfg",
             "0.0.0.0:6018",
             "0.0.0.0:5018",
             "/tmp/edgy-xahau.log",
             "xahau"},
        };
        auto const repoCfg = std::filesystem::path(__FILE__).parent_path().parent_path();
        for (auto const& spec : shipped)
        {
            bool parsed = false;
            std::string const fromRepo = (repoCfg / spec.rel).string();
            for (char const* p : {fromRepo.c_str(), spec.rel, spec.abs})
            {
                std::ifstream probe(p);
                if (!probe)
                    continue;
                auto const cfg = Config::fromFile(p);
                auto listenWs = std::string{"shipped [listen-ws] "} + spec.label;
                auto listenRpc = std::string{"shipped [listen-rpc] "} + spec.label;
                auto node = std::string{"shipped [node] "} + spec.label;
                auto debug = std::string{"shipped [debug] "} + spec.label;
                auto search = std::string{"shipped [search] full "} + spec.label;
                auto searchFast = std::string{"shipped [search-fast] fast "} + spec.label;
                auto snapshot = std::string{"shipped [full-snapshot] full "} + spec.label;
                expect(cfg.listenWs == spec.listenWs, listenWs.c_str());
                expect(cfg.listenRpc == spec.listenRpc, listenRpc.c_str());
                expect(cfg.nodeWs == "ws://127.0.0.1:6006", node.c_str());
                expect(
                    cfg.debugLog == spec.debug ||
                        cfg.debugLog == (std::string{"/private"} + spec.debug),
                    debug.c_str());
                expect(cfg.search == Config::kSearchFull, search.c_str());
                expect(cfg.searchFast == Config::kSearchFast, searchFast.c_str());
                expect(cfg.fullSnapshot, snapshot.c_str());
                auto snapPage = std::string{"shipped [snapshot-page] 2048 "} + spec.label;
                expect(cfg.snapshotPage == 2048, snapPage.c_str());
                auto workers = std::string{"shipped [workers] "} + spec.label;
                auto proxy = std::string{"shipped [proxy] "} + spec.label;
                auto update = std::string{"shipped [update-ms] "} + spec.label;
                expect(cfg.workers == 64, workers.c_str());
                expect(cfg.proxyOther, proxy.c_str());
                expect(cfg.midCloseDelay.count() == 100, update.c_str());
                parsed = true;
                break;
            }
            auto parsedWhat = std::string{"shipped edgy-"} + spec.label + ".example.cfg parses";
            expect(parsed, parsedWhat.c_str());
        }
    }


    return edgy::test::finish("config");
}
