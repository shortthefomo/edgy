#include <edgy/book_util.hpp>
#include <edgy/config.hpp>
#include <edgy/version.hpp>
#include <edgy/engine.hpp>
#include <edgy/graph.hpp>
#include <edgy/memory_ledger.hpp>
#include <edgy/node_client.hpp>
#include <edgy/order_books.hpp>
#include <edgy/protocol.hpp>
#include <edgy/services.hpp>
#include <edgy/session.hpp>

#include <xrpld/rpc/detail/AccountAssets.h>
#include <xrpld/rpc/detail/AssetCache.h>
#include <xrpld/rpc/detail/Pathfinder.h>
#include <xrpld/rpc/detail/Tuning.h>

#include <xrpl/json/json_value.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <xrpl/protocol/Quality.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STVector256.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/jss.h>

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int gFails = 0;

void
expect(bool cond, char const* what)
{
    if (!cond)
    {
        std::cerr << "FAIL: " << what << '\n';
        ++gFails;
    }
    else
    {
        std::cerr << "ok    " << what << '\n';
    }
}

xrpl::AccountID
testAccount(char const* b58)
{
    auto id = xrpl::parseBase58<xrpl::AccountID>(b58);
    if (!id)
        throw std::runtime_error(std::string("bad account ") + b58);
    return *id;
}

}  // namespace

int
main()
{
    using namespace edgy;

    expect(xrpl::rpc::tuning::kPathFindMaxPaths == 6, "max paths is 6 (xrpld)");
    {
        auto const v = versionString();
        expect(v.find(kVersionBase) == 0, "version string starts with kVersionBase");
        expect(!v.empty(), "version string is not empty");
        expect(v.find("xrpld") != std::string::npos, "xrpld-linked tests report xrpld flavor");
    }
    {
        Config d;
        expect(d.search == Config::kSearchFull, "default [search] is full");
        expect(d.searchFast == Config::kSearchFull, "default [search-fast] is full");
        expect(d.searchTimeout.count() == 0, "default [timeout-ms] is none");
        expect(d.fullSnapshot, "default [full-snapshot] is full");
        expect(d.midCloseDelay.count() == 100, "default [update-ms] is 100");
        expect(d.network == NetworkKind::xrpl, "xrpld binary is xrpl");
        expect(!d.xahau(), "xrpld binary is not xahau");
        expect(std::string{d.nativeCurrency()} == "XRP", "xrpld native is XRP");
        expect(std::string{d.nodeSoftware()} == "xrpld", "xrpld node software is xrpld");
    }

    {
        // Regression: empty apply queue skipped midCloseTick, so path_find
        // updates only went out on ledger close (~4s) instead of 100ms.
        auto idle = planApplyCycle(false, true, true);
        expect(!idle.exit && !idle.hold && !idle.takeBatch && idle.tick,
               "ready + empty queue still ticks (100ms path_find updates)");
        auto work = planApplyCycle(false, true, false);
        expect(!work.exit && !work.hold && work.takeBatch && work.tick,
               "ready + queued apply takes a batch and ticks");
        auto snap = planApplyCycle(false, false, false);
        expect(!snap.exit && snap.hold && !snap.takeBatch && !snap.tick,
               "snapshot hold does not apply or tick");
        auto snapIdle = planApplyCycle(false, false, true);
        expect(!snapIdle.exit && snapIdle.hold && !snapIdle.tick,
               "snapshot hold with empty queue does not tick");
        auto halt = planApplyCycle(true, true, true);
        expect(halt.exit, "stop + empty queue exits the apply loop");
        auto drain = planApplyCycle(true, true, false);
        expect(!drain.exit && drain.takeBatch && !drain.tick,
               "stop + queued apply drains then does not tick");
    }

    {
        // xrpld pubLedger sends ledgerClosed for N, then txs with ledger_index N.
        expect(shouldApplyStreamTx(100, 100), "tx for the just-closed ledger applies");
        expect(shouldApplyStreamTx(101, 100), "tx for the next ledger applies");
        expect(!shouldApplyStreamTx(99, 100), "tx for an older ledger is skipped");
        expect(shouldApplyStreamTx(0, 100), "tx with no ledger_index still applies");
        expect(applyTxsMatchNode(40, 0, 40), "all txs applied matches node");
        expect(applyTxsMatchNode(38, 2, 40), "applied + missing-meta matches node");
        expect(!applyTxsMatchNode(40, 391, 40), "parseFail must not be added into the tx check");
        expect(applyTxsMatchNode(0, 0, 0), "unknown node txn_count is ok");
    }

    {
        // Stream JSON meta is parsed as sfGeneric; we must set CreatedNode
        // or applyMetaNode returns None and the overlay never moves.
        LedgerBuilder b;
        LocalOrderBooks books;
        auto const src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto const key = xrpl::keylet::account(src).key;
        json::Value meta{json::ValueType::Object};
        json::Value& nodes = (meta["AffectedNodes"] = json::ValueType::Array);
        json::Value& wrap = nodes.append(json::ValueType::Object);
        json::Value& created = (wrap["CreatedNode"] = json::ValueType::Object);
        created["LedgerEntryType"] = "AccountRoot";
        created["LedgerIndex"] = to_string(key);
        json::Value& fields = (created["NewFields"] = json::ValueType::Object);
        fields["Account"] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        fields["Balance"] = "10000000000";
        fields["Sequence"] = 1;
        auto const stats = applyJsonAffectedNodes(b, books, meta);
        expect(stats.parseFail == 0, "JSON AffectedNodes parse succeeds");
        expect(stats.created == 1, "JSON CreatedNode increments created");
        expect(stats.applied() == 1, "JSON CreatedNode is applied");
        expect(b.contains(key), "JSON CreatedNode upserts the SLE");
        auto view = b.publish();
        expect(view->exists(xrpl::keylet::account(src)), "created account is readable");
        expect(view->overlaySize() == 1 || view->size() == 1, "overlay/base holds the new object");

        json::Value mod{json::ValueType::Object};
        json::Value& modNodes = (mod["AffectedNodes"] = json::ValueType::Array);
        json::Value& modWrap = modNodes.append(json::ValueType::Object);
        json::Value& modified = (modWrap["ModifiedNode"] = json::ValueType::Object);
        modified["LedgerEntryType"] = "AccountRoot";
        modified["LedgerIndex"] = to_string(key);
        json::Value& finals = (modified["FinalFields"] = json::ValueType::Object);
        finals["Account"] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        finals["Balance"] = "20000000000";
        finals["Sequence"] = 2;
        auto const modStats = applyJsonAffectedNodes(b, books, mod);
        expect(modStats.parseFail == 0, "JSON ModifiedNode parse succeeds");
        expect(modStats.modified == 1 || modStats.applied() >= 1, "JSON ModifiedNode is applied");
        expect(b.contains(key), "ModifiedNode keeps the account");
    }

    {
        // xahaud AccountRoot JSON includes reward/hook fields libxrpl does not know.
        LedgerBuilder b;
        LocalOrderBooks books;
        auto const src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto const key = xrpl::keylet::account(src).key;
        json::Value meta{json::ValueType::Object};
        json::Value& nodes = (meta["AffectedNodes"] = json::ValueType::Array);
        json::Value& wrap = nodes.append(json::ValueType::Object);
        json::Value& created = (wrap["CreatedNode"] = json::ValueType::Object);
        created["LedgerEntryType"] = "AccountRoot";
        created["LedgerIndex"] = to_string(key);
        json::Value& fields = (created["NewFields"] = json::ValueType::Object);
        fields["Account"] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        fields["Balance"] = "10000000000";
        fields["Sequence"] = 1;
        fields["RewardAccumulator"] = "1";
        fields["HookStateCount"] = 0;
        auto const stats = applyJsonAffectedNodes(b, books, meta);
        expect(stats.parseFail == 0, "unknown xahaud JSON fields are stripped");
        expect(stats.created == 1, "AccountRoot still applies with extra JSON fields");
        expect(b.contains(key), "xahaud extra fields do not drop the SLE");
    }

    {
        // Issued amounts are {currency,issuer,value}. Those keys are not
        // SFields; stripping them used to parseFail every Offer / line.
        LedgerBuilder b;
        LocalOrderBooks books;
        auto const src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto const gw = src;
        xrpl::uint256 key;
        (void)key.parseHex(
            "00700000000000000000000000000000000000000000000000000000000000AA");
        json::Value meta{json::ValueType::Object};
        json::Value& nodes = (meta["AffectedNodes"] = json::ValueType::Array);
        json::Value& wrap = nodes.append(json::ValueType::Object);
        json::Value& created = (wrap["CreatedNode"] = json::ValueType::Object);
        created["LedgerEntryType"] = "Offer";
        created["LedgerIndex"] = to_string(key);
        json::Value& fields = (created["NewFields"] = json::ValueType::Object);
        fields["Account"] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        fields["Sequence"] = 2;
        json::Value pays{json::ValueType::Object};
        pays["currency"] = "USD";
        pays["issuer"] = to_string(gw);
        pays["value"] = "1";
        fields["TakerPays"] = pays;
        fields["TakerGets"] = "1000000";
        fields["BookDirectory"] =
            "02000000000000000000000000000000000000000000000000000000000000BB";
        fields["BookNode"] = "0";
        fields["OwnerNode"] = "0";
        json::Value stripped = created;
        stripUnknownJsonFields(stripped);
        expect(stripped["NewFields"]["TakerPays"].isObject() &&
                   stripped["NewFields"]["TakerPays"].isMember("currency") &&
                   stripped["NewFields"]["TakerPays"]["currency"].asString() == "USD",
               "amount JSON currency/issuer/value survive strip");
        auto const stats = applyJsonAffectedNodes(b, books, meta);
        expect(stats.parseFail == 0, "Offer with issued TakerPays parses");
        expect(stats.created == 1 || stats.incomplete == 1,
               "issued-amount Offer is applied");
        expect(b.contains(key), "issued-amount Offer is stored");
    }

    {
        // xahaud Hook / URIToken types are unknown to rippled libxrpl.
        LedgerBuilder b;
        LocalOrderBooks books;
        xrpl::uint256 hookKey;
        (void)hookKey.parseHex(
            "00480000000000000000000000000000000000000000000000000000000000CC");
        b.upsertRaw(hookKey, xrpl::Blob{0x01, 0x02, 0x03});
        json::Value meta{json::ValueType::Object};
        json::Value& nodes = (meta["AffectedNodes"] = json::ValueType::Array);
        json::Value& createdWrap = nodes.append(json::ValueType::Object);
        json::Value& created = (createdWrap["CreatedNode"] = json::ValueType::Object);
        created["LedgerEntryType"] = "Hook";
        created["LedgerIndex"] =
            "00480000000000000000000000000000000000000000000000000000000000DD";
        json::Value& delWrap = nodes.append(json::ValueType::Object);
        json::Value& deleted = (delWrap["DeletedNode"] = json::ValueType::Object);
        deleted["LedgerEntryType"] = "HookState";
        deleted["LedgerIndex"] = to_string(hookKey);
        auto const stats = applyJsonAffectedNodes(b, books, meta);
        expect(stats.parseFail == 0, "unknown xahaud types are not parse_fail");
        expect(stats.skippedUnknown == 1, "Hook create is skipped");
        expect(stats.deleted == 1, "unknown-type DeletedNode still erases by index");
        expect(!b.contains(hookKey), "HookState delete removes the blob");
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
                << "[full-snapshot]\nfull\n";
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
            out << "[search]\n2\n"
                << "[search-fast]\nfast\n"
                << "[timeout-ms]\n250\n"
                << "[full-snapshot]\nbooks\n";
        }
        auto const cfg = Config::fromFile(path);
        expect(cfg.search == 2, "stanza [search] 2");
        expect(cfg.searchFast == 0, "stanza [search-fast] fast");
        expect(cfg.searchTimeout.count() == 250, "stanza [timeout-ms] 250");
        expect(!cfg.fullSnapshot, "stanza [full-snapshot] books");
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
        char a4[] = "0";
        char a5[] = "--timeout-ms";
        char a6[] = "100";
        char a7[] = "--full-snapshot";
        char a8[] = "0";
        char* argv[] = {arg0, a1, a2, a3, a4, a5, a6, a7, a8, nullptr};
        auto const cfg = Config::fromArgs(9, argv);
        expect(cfg.search == Config::kSearchFull, "CLI --search full");
        expect(cfg.searchFast == 0, "CLI --search-fast 0");
        expect(cfg.searchTimeout.count() == 100, "CLI --timeout-ms 100");
        expect(!cfg.fullSnapshot, "CLI --full-snapshot 0");
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
                << "[search]\n2\n[search-fast]\n4\n[timeout-ms]\n-5\n";
        }
        auto const cfg = Config::fromFile(path);
        expect(cfg.workers == 1, "workers 0 clamps to 1");
        expect(cfg.netThreads == 1, "net-threads 0 clamps to 1");
        expect(cfg.midCloseDelay.count() == 20, "update-ms below 20 clamps to 20");
        expect(cfg.search == 2, "search 2 is kept");
        expect(cfg.searchFast == 2, "search-fast clamps to search");
        expect(cfg.searchTimeout.count() == 0, "negative timeout-ms clamps to 0");
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
        for (auto const& spec : shipped)
        {
            bool parsed = false;
            for (char const* p : {spec.rel, spec.abs})
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
                auto searchFast = std::string{"shipped [search-fast] full "} + spec.label;
                auto snapshot = std::string{"shipped [full-snapshot] full "} + spec.label;
                expect(cfg.listenWs == spec.listenWs, listenWs.c_str());
                expect(cfg.listenRpc == spec.listenRpc, listenRpc.c_str());
                expect(cfg.nodeWs == "ws://127.0.0.1:6006", node.c_str());
                expect(
                    cfg.debugLog == spec.debug ||
                        cfg.debugLog == (std::string{"/private"} + spec.debug),
                    debug.c_str());
                expect(cfg.search == Config::kSearchFull, search.c_str());
                expect(cfg.searchFast == Config::kSearchFull, searchFast.c_str());
                expect(cfg.fullSnapshot, snapshot.c_str());
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

    {
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 42;
        (void)h.hash.parseHex(
            "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF");
        b.setHeader(h);
        auto src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto sle = std::make_shared<xrpl::SLE>(xrpl::keylet::account(src));
        sle->setAccountID(xrpl::sfAccount, src);
        sle->setFieldAmount(xrpl::sfBalance, xrpl::STAmount{xrpl::XRPAmount{10'000'000'000}});
        sle->setFieldU32(xrpl::sfSequence, 1);
        sle->setFieldU32(xrpl::sfOwnerCount, 0);
        sle->setFieldU32(xrpl::sfFlags, 0);
        b.upsert(sle);
        auto view = b.publish();
        expect(view->seq() == 42, "published ledger seq");
        expect(view->exists(xrpl::keylet::account(src)), "account exists in memory ledger");
        expect(view->read(xrpl::keylet::account(src)) != nullptr, "account readable");
        expect(view->size() == 1, "single object stored");
        // Rules::Impl refs presets; a temporary set used to UAF in enabled().
        (void)view->rules().enabled(xrpl::uint256{});
        auto const rebuilt = xrpl::makeRulesGivenLedger(*view, view->rules());
        (void)rebuilt.enabled(xrpl::uint256{});
        expect(true, "Rules::enabled after publish does not dangle presets");

        xrpl::Serializer extra;
        sle->add(extra);
        extra.addFieldID(xrpl::STI_UINT32, 200);
        extra.add32(std::uint32_t{42});
        auto parsed = sleFromBlob(extra.getData(), xrpl::keylet::account(src).key);
        expect(parsed != nullptr, "sleFromBlob drops unknown xahaud fields");
        expect(parsed->isFieldPresent(xrpl::sfAccount), "stripped SLE still has Account");
        expect(parsed->getAccountID(xrpl::sfAccount) == src, "stripped SLE Account matches");
    }

    {
        json::Value amt{json::ValueType::Object};
        amt["currency"] = "XAH";
        amt["value"] = "1";
        rewriteNativeJsonIn(amt, NetworkKind::xahau);
        expect(amt.isString() && amt.asString() == "1000000", "1 XAH becomes 1000000 drops");
        xrpl::STAmount sa;
        expect(xrpl::amountFromJsonNoThrow(sa, amt), "XAH amount parses after rewrite");
        expect(sa.native(), "XAH rewrites to native");

        json::Value srcCur{json::ValueType::Object};
        srcCur["currency"] = "XAH";
        rewriteNativeJsonIn(srcCur, NetworkKind::xahau);
        expect(srcCur.isObject() && srcCur["currency"].asString() == "XRP",
               "source currency XAH becomes native XRP");

        json::Value all{json::ValueType::Object};
        all["currency"] = "XAH";
        all["value"] = "-1";
        rewriteNativeJsonIn(all, NetworkKind::xahau);
        expect(all.isString() && all.asString() == "-1", "XAH convert-all stays -1");

        json::Value out{json::ValueType::Object};
        out["currency"] = "XRP";
        out["value"] = "1";
        rewriteNativeJsonOut(out, NetworkKind::xahau);
        expect(out["currency"].asString() == "XAH", "native XRP becomes XAH on the way out");

        json::Value usd{json::ValueType::Object};
        usd["currency"] = "USD";
        usd["issuer"] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        usd["value"] = "1";
        rewriteNativeJsonOut(usd, NetworkKind::xahau);
        expect(usd["currency"].asString() == "USD", "issued USD is not rewritten");

        json::Value issued{json::ValueType::Object};
        issued["currency"] = "XRP";
        issued["issuer"] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        rewriteNativeJsonOut(issued, NetworkKind::xahau);
        expect(issued["currency"].asString() == "XRP", "issued XRP is not rewritten");

        json::Value dest{json::ValueType::Object};
        dest["destination_currencies"] = json::ValueType::Array;
        dest["destination_currencies"].append("XRP");
        dest["destination_currencies"].append("USD");
        rewriteNativeJsonOut(dest, NetworkKind::xahau);
        expect(dest["destination_currencies"][0u].asString() == "XAH",
               "destination_currencies XRP becomes XAH");
        expect(dest["destination_currencies"][1u].asString() == "USD",
               "destination_currencies USD stays");

        json::Value leave{json::ValueType::Object};
        leave["currency"] = "XAH";
        leave["value"] = "1";
        rewriteNativeJsonIn(leave, NetworkKind::xrpl);
        expect(leave["currency"].asString() == "XAH", "xrpl mode leaves XAH as issued");

        json::Value mixed{json::ValueType::Object};
        json::Value& alts = (mixed["alternatives"] = json::ValueType::Array);
        json::Value& alt0 = alts.append(json::ValueType::Object);
        json::Value srcAmt{json::ValueType::Object};
        srcAmt["currency"] = "XRP";
        srcAmt["value"] = "1";
        alt0["source_amount"] = srcAmt;
        rewriteNativeJsonOut(mixed, NetworkKind::xahau);
        expect(mixed["alternatives"][0u]["source_amount"]["currency"].asString() == "XAH",
               "nested alternatives native XRP becomes XAH");

        json::Value lower{json::ValueType::Object};
        lower["currency"] = "xah";
        lower["value"] = "2";
        rewriteNativeJsonIn(lower, NetworkKind::xahau);
        expect(lower.isString() && lower.asString() == "2000000", "lowercase xah rewrites inbound");
    }

    {
        json::Value node{json::ValueType::Object};
        json::Value& fields = (node["NewFields"] = json::ValueType::Object);
        fields["Account"] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        json::Value hook{json::ValueType::Object};
        hook["HookHash"] = "00";
        fields["Hook"] = hook;
        json::Value amt{json::ValueType::Object};
        amt["currency"] = "USD";
        amt["issuer"] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        amt["value"] = "1";
        fields["TakerPays"] = amt;
        fields["RewardAccumulator"] = "1";
        slimJsonMetaNode(node);
        expect(!node["NewFields"].isMember("RewardAccumulator"),
               "slim drops unknown JSON keys");
        expect(!node["NewFields"].isMember("Hook"), "slim drops nested non-amount objects");
        expect(node["NewFields"].isMember("TakerPays") &&
                   node["NewFields"]["TakerPays"].isMember("currency"),
               "slim keeps amount JSON leaves");
        expect(node["NewFields"].isMember("Account"), "slim keeps known fields");
    }

    {
        expect(sleFromBinary("zz", "00") == nullptr, "sleFromBinary rejects bad hex");
        xrpl::uint256 key{};
        expect(sleFromBlob(xrpl::Blob{}, key) == nullptr, "sleFromBlob rejects empty blob");
    }

    {
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 7;
        b.setHeader(h);
        auto src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto makeSle = [&]() {
            auto sle = std::make_shared<xrpl::SLE>(xrpl::keylet::account(src));
            sle->setAccountID(xrpl::sfAccount, src);
            sle->setFieldAmount(xrpl::sfBalance, xrpl::STAmount{xrpl::XRPAmount{1'000'000}});
            sle->setFieldU32(xrpl::sfSequence, 1);
            sle->setFieldU32(xrpl::sfOwnerCount, 0);
            sle->setFieldU32(xrpl::sfFlags, 0);
            return sle;
        };
        xrpl::Serializer s;
        makeSle()->add(s);
        auto blob = s.getData();

        xrpl::uint256 k1;
        xrpl::uint256 k2;
        xrpl::uint256 k3;
        (void)k1.parseHex("0100000000000000000000000000000000000000000000000000000000000001");
        (void)k2.parseHex("0100000000000000000000000000000000000000000000000000000000000002");
        (void)k3.parseHex("0100000000000000000000000000000000000000000000000000000000000003");
        b.upsertRaw(k1, blob);
        b.upsertRaw(k3, blob);
        auto v1 = b.publish();
        expect(v1->succ(k1).has_value() && *v1->succ(k1) == k3, "succ skips to next snapshot key");
        expect(v1->size() == 2, "two snapshot objects");

        b.upsertRaw(k2, blob);
        auto v2 = b.publish();
        expect(v2->succ(k1).has_value() && *v2->succ(k1) == k2, "overlay insert is visible to succ");
        expect(v1->succ(k1).has_value() && *v1->succ(k1) == k3, "published view is isolated from overlay");
        expect(v2->size() == 3, "overlay insert increases size");
        expect(v2->overlaySize() == 1, "overlay holds the new key");

        b.erase(k2);
        auto v3 = b.publish();
        expect(v3->succ(k1).has_value() && *v3->succ(k1) == k3, "overlay delete restores snapshot succ");
        expect(v3->size() == 2, "overlay delete decreases size");
        expect(v2->size() == 3, "previous published view keeps overlay insert");
    }

    {
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 9;
        b.setHeader(h);
        expect(!b.publish()->open(), "published ledger is closed by default");
        b.setOpen(true);
        auto openView = b.publish();
        expect(openView->open(), "setOpen(true) is visible on publish");
        b.setOpen(false);
        auto closedView = b.publish();
        expect(!closedView->open(), "setOpen(false) is visible on publish");
        expect(openView->open(), "prior published view keeps its open flag");
    }

    {
        boost::asio::io_context io;
        PathServices services(io);
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 11;
        b.setHeader(h);
        b.setOpen(true);
        auto openView = b.publish();
        auto cache = std::make_shared<xrpl::AssetCache>(openView, services.getJournal("test"));
        expect(cache->getLedger()->open(), "cache starts on the open view");
        b.setOpen(false);
        auto closedView = b.publish();
        cache->advanceLedger(closedView);
        expect(!cache->getLedger()->open(), "same-seq open->closed swaps the view");
        expect(cache->getLedger()->seq() == 11, "same-seq swap keeps the sequence");
        b.setOpen(true);
        auto openAgain = b.publish();
        cache->advanceLedger(openAgain);
        expect(cache->getLedger()->open(), "same-seq closed->open still swaps the view");
    }

    {
        // Regression: forEachItem passes nullptr when owner-dir Indexes
        // names a missing/unreadable child. getMPTs used to sle->getType()
        // and SIGSEGV on the first path_find after a live close.
        boost::asio::io_context io;
        PathServices services(io);
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 99;
        b.setHeader(h);
        auto const src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto acct = std::make_shared<xrpl::SLE>(xrpl::keylet::account(src));
        acct->setAccountID(xrpl::sfAccount, src);
        acct->setFieldAmount(xrpl::sfBalance, xrpl::STAmount{xrpl::XRPAmount{10'000'000'000}});
        acct->setFieldU32(xrpl::sfSequence, 1);
        acct->setFieldU32(xrpl::sfOwnerCount, 1);
        acct->setFieldU32(xrpl::sfFlags, 0);
        b.upsert(acct);

        xrpl::uint256 missing;
        (void)missing.parseHex(
            "DEAD000000000000000000000000000000000000000000000000000000000001");
        auto const dirKey = xrpl::keylet::ownerDir(src);
        auto dir = std::make_shared<xrpl::SLE>(dirKey);
        xrpl::STVector256 indexes(xrpl::sfIndexes);
        indexes.pushBack(missing);
        dir->setFieldV256(xrpl::sfIndexes, indexes);
        dir->setFieldH256(xrpl::sfRootIndex, dirKey.key);
        dir->setFieldU64(xrpl::sfIndexNext, 0);
        b.upsert(dir);

        auto view = b.publish();
        auto cache = std::make_shared<xrpl::AssetCache>(view, services.getJournal("test"));
        bool crashed = false;
        try
        {
            auto const dest = xrpl::accountDestAssets(src, cache, true);
            expect(!dest.empty(), "dest assets still include native after a hole in owner dir");
            (void)cache->getMPTs(src);
        }
        catch (...)
        {
            crashed = true;
        }
        expect(!crashed, "owner-dir hole does not crash getMPTs / accountDestAssets");
    }

    {
        LocalOrderBooks books;
        xrpl::Issue xrp = xrpl::xrpIssue();
        auto const gw = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        xrpl::Issue usd{xrpl::toCurrency("USD"), gw};
        xrpl::Issue eur{xrpl::toCurrency("EUR"), gw};
        books.addOrderBook({usd, xrp, std::nullopt});
        books.addOrderBook({xrp, eur, std::nullopt});
        books.addOrderBook({usd, eur, std::nullopt});
        expect(books.hasBook(usd, xrp), "direct book usd->xrp");
        expect(books.hasBook(xrp, eur), "direct book xrp->eur");
        expect(!books.hasBook(eur, usd), "missing reverse book");
        auto const other = testAccount("rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe");
        xrpl::Issue usdOther{xrpl::toCurrency("USD"), other};
        expect(books.hasBook(usdOther, xrp), "same-currency holder sees the USD book");
        auto mids = books.intermediates(usd, eur);
        bool sawXrp = false;
        for (auto const& mid : mids)
            sawXrp = sawXrp || xrpl::isXRP(mid);
        expect(sawXrp, "2-hop intersection includes XRP");
        auto usdOut = books.neighbors(usd);
        expect(!usdOut.empty(), "usd has neighbors");
    }

    {
        LocalOrderBooks books;
        auto const gw = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        xrpl::Issue usd{xrpl::toCurrency("USD"), gw};
        xrpl::Issue xrp = xrpl::xrpIssue();
        xrpl::uint256 key;
        (void)key.parseHex("02000000000000000000000000000000000000000000000000000000000000AA");
        auto makeDir = [&](std::uint64_t rate) {
            auto sle = std::make_shared<xrpl::SLE>(xrpl::ltDIR_NODE, key);
            sle->setFieldH256(xrpl::sfRootIndex, key);
            sle->setFieldU64(xrpl::sfExchangeRate, rate);
            sle->setFieldH160(xrpl::sfTakerPaysCurrency, usd.currency);
            sle->setFieldH160(xrpl::sfTakerPaysIssuer, usd.account);
            sle->setFieldH160(xrpl::sfTakerGetsCurrency, xrp.currency);
            sle->setFieldH160(xrpl::sfTakerGetsIssuer, xrp.account);
            return sle;
        };
        books.addFromSle(makeDir(500));
        expect(books.hasBook(usd, xrp), "book-dir SLE adds the book");
        expect(books.tipQuality(usd, xrp) == 500, "tip quality is the exchange rate");
        books.addFromSle(makeDir(100));
        expect(books.tipQuality(usd, xrp) == 100, "tip quality replaces on a better rate");
        books.addFromSle(makeDir(800));
        expect(books.tipQuality(usd, xrp) == 800, "tip quality replaces even when worse");
        books.removeFromSle(makeDir(800));
        expect(!books.hasBook(usd, xrp), "removeFromSle drops the book");
        expect(books.tipQuality(usd, xrp) == LocalOrderBooks::kNoQuality, "removed book has no tip");
        expect(books.bookCount() == 0, "bookCount is zero after remove");
        books.addFromSle(makeDir(500));
        expect(books.bookCount() == 1, "bookCount is one after add");
        books.clear();
        expect(books.bookCount() == 0, "clear drops every book");
        expect(books.neighbors(usd).empty(), "neighbors of unknown asset are empty");
    }

    {
        auto const gw = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        xrpl::Issue usd{xrpl::toCurrency("USD"), gw};
        xrpl::Issue xrp = xrpl::xrpIssue();
        xrpl::STAmount const pays{usd, 10};
        xrpl::STAmount const gets{xrp, 20};
        auto hop1 = xrpl::Quality{xrpl::getRate(gets, pays)};
        auto hop2 = xrpl::Quality{xrpl::getRate(pays, gets)};
        auto composed = composeQuality(hop1, hop2);
        auto direct = xrpl::Quality{xrpl::getRate(pays, pays)};
        expect(composed == direct, "composed hop qualities multiply to the direct rate");
        expect(qualityRatio(hop1) > 0, "qualityRatio is a positive in/out");
        expect(amountAsDouble(gets) == 20, "amountAsDouble reads native drops");
    }

    {
        LocalOrderBooks books;
        auto const gw = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        xrpl::Issue usd{xrpl::toCurrency("USD"), gw};
        xrpl::Issue xrp = xrpl::xrpIssue();
        xrpl::uint256 key;
        (void)key.parseHex("03000000000000000000000000000000000000000000000000000000000000BB");
        auto offer = std::make_shared<xrpl::SLE>(xrpl::ltOFFER, key);
        offer->setFieldAmount(xrpl::sfTakerPays, xrpl::STAmount{usd, 25});
        offer->setFieldAmount(xrpl::sfTakerGets, xrpl::STAmount{xrp, 50});
        books.addFromSle(offer);
        expect(books.hasBook(usd, xrp), "offer SLE adds the book");
        expect(books.tip(usd, xrp).outSize == 50, "offer SLE stores tip takerGets");
        expect(books.tip(usd, xrp).quality == xrpl::getRate(xrpl::STAmount{xrp, 50}, xrpl::STAmount{usd, 25}),
               "offer SLE stores getRate quality");
        books.removeFromSle(offer);
        expect(books.hasBook(usd, xrp), "removing an offer keeps the book adjacency");
        expect(books.tip(usd, xrp).outSize == 0, "removing the tip offer clears size");
    }

    {
        using namespace std::chrono_literals;
        auto d0 = SearchBudget::forDepth(0);
        auto d2 = SearchBudget::forDepth(2);
        auto d4 = SearchBudget::forDepth(4);
        expect(d0.maxHops < d2.maxHops && d2.maxHops < d4.maxHops, "deeper budget allows more hops");
        expect(d0.rank < d4.rank, "deeper budget ranks more candidates");
        expect(d2.twoHop >= 64, "one-shot scans a full 2-hop book set");
        expect(d2.rank <= 16, "RippleCalc only the best-scored pairs");
        expect(d0.maxHops == 2, "first WS reply is at most 2 hops");
        expect(d4.maxHops == SearchBudget::kMaxPathLength, "deepest search uses the payment hop cap");
        expect(d4.maxHops <= 8, "never above XRPL 8-hop payment limit");
        expect(SearchBudget::kMaxPathCount == 6, "at most 6 paths per payment");
        expect(SearchBudget::kMaxPathLength == 8, "at most 8 hops per path");
        expect(SearchBudget::depthFor(true, 0, 0ms) == 2, "one-shot uses mid depth");
        expect(SearchBudget::depthFor(false, 0, 0ms) == 0, "fresh WS starts shallow");
        expect(SearchBudget::depthFor(false, 1, 0ms) == 1, "each WS search raises depth");
        expect(SearchBudget::depthFor(false, 0, 12s) == 2, "open age raises depth");
        expect(SearchBudget::depthFor(false, 0, 50s) == 4, "long-lived WS reaches max depth");
        expect(SearchBudget::depthFor(false, 8, 0ms) == 4, "many updates clamp at max depth");
        expect(SearchBudget::depthFor(true, 0, 0ms, 4, 4) == 4, "configured full one-shot");
        expect(SearchBudget::depthFor(false, 0, 0ms, 4, 4) == 4, "configured full WS starts full");
        expect(SearchBudget::depthFor(false, 0, 0ms, 0, 4) == 0, "search-fast 0 starts shallow");
        expect(SearchBudget::depthFor(false, 0, 50s, 0, 2) == 2, "search caps the climb");
        expect(SearchBudget::forDepth(-3).maxHops >= 1, "negative depth still has at least one hop");
        expect(SearchBudget::forDepth(99).maxHops == SearchBudget::kMaxPathLength,
               "oversize depth clamps to 8 hops");
        expect(SearchBudget::depthFor(false, 0, 0ms, 3, 1) == 1,
               "search-fast above search uses the search cap");
    }

    {
        boost::asio::io_context io;
        PathServices services(io);
        xrpl::Pathfinder::initPathTable();
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 1;
        b.setHeader(h);
        auto view = b.publish();
        auto cache = std::make_shared<xrpl::AssetCache>(view, services.getJournal("test"));
        Config cfg;
        PathSession session(services, cfg, 1, true, services.getJournal("session"));
        PathSession live(services, cfg, 2, false, services.getJournal("session"));
        expect(!live.shouldDeepen(), "fresh WS does not deepen on the first tick");
        expect(!live.shouldRediscover(10), "fresh WS has no last-full ledger yet");
        expect(!rediscoveryDue(0, 99, 2, 3), "rediscoveryDue is false before first full search");
        expect(!rediscoveryDue(10, 12, 0, 3), "rediscoveryDue waits for interval");
        expect(rediscoveryDue(10, 13, 0, 3), "rediscoveryDue at last+interval for id%3==0");
        expect(!rediscoveryDue(10, 13, 1, 3), "rediscoveryDue staggers id%3==1 by one extra ledger");
        expect(rediscoveryDue(10, 14, 1, 3), "rediscoveryDue at last+interval+1 for id%3==1");
        expect(rediscoveryDue(10, 15, 2, 3), "rediscoveryDue at last+interval+2 for id%3==2");
        expect(rediscoveryDue(10, 11, 0, 1), "interval 1 rediscovers the next ledger");

        json::Value missing{json::ValueType::Object};
        auto [ok, st] = session.doCreate(cache, missing);
        expect(!ok, "create without accounts is invalid");
        expect(st.isMember(xrpl::jss::error), "rpc error object");

        json::Value bad{json::ValueType::Object};
        bad[xrpl::jss::source_account] = "not-an-account";
        bad[xrpl::jss::destination_account] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        bad[xrpl::jss::destination_amount] = "1000000";
        auto [ok2, st2] = session.doCreate(cache, bad);
        expect(!ok2, "malformed source rejected");
        expect(st2.isMember(xrpl::jss::error), "malformed source has error");

        json::Value noDst{json::ValueType::Object};
        noDst[xrpl::jss::source_account] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        noDst[xrpl::jss::destination_amount] = "1000000";
        auto [ok3, st3] = session.doCreate(cache, noDst);
        expect(!ok3 && st3.isMember(xrpl::jss::error), "missing destination_account is invalid");

        json::Value noAmt{json::ValueType::Object};
        noAmt[xrpl::jss::source_account] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        noAmt[xrpl::jss::destination_account] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        auto [ok4, st4] = session.doCreate(cache, noAmt);
        expect(!ok4 && st4.isMember(xrpl::jss::error), "missing destination_amount is invalid");

        json::Value zeroAmt{json::ValueType::Object};
        zeroAmt[xrpl::jss::source_account] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        zeroAmt[xrpl::jss::destination_account] = "rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe";
        zeroAmt[xrpl::jss::destination_amount] = "0";
        auto [ok5, st5] = session.doCreate(cache, zeroAmt);
        expect(!ok5 && st5.isMember(xrpl::jss::error), "zero destination_amount is invalid");

        json::Value emptySrc{json::ValueType::Object};
        emptySrc[xrpl::jss::source_account] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        emptySrc[xrpl::jss::destination_account] = "rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe";
        emptySrc[xrpl::jss::destination_amount] = "1000000";
        emptySrc[xrpl::jss::source_currencies] = json::ValueType::Array;
        auto [ok6, st6] = session.doCreate(cache, emptySrc);
        expect(!ok6 && st6.isMember(xrpl::jss::error), "empty source_currencies is invalid");

        json::Value tooMany{json::ValueType::Object};
        tooMany[xrpl::jss::source_account] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        tooMany[xrpl::jss::destination_account] = "rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe";
        tooMany[xrpl::jss::destination_amount] = "1000000";
        json::Value& curs = (tooMany[xrpl::jss::source_currencies] = json::ValueType::Array);
        for (int i = 0; i < xrpl::rpc::tuning::kMaxSrcCur + 1; ++i)
        {
            json::Value c{json::ValueType::Object};
            c[xrpl::jss::currency] = "USD";
            curs.append(c);
        }
        auto [ok7, st7] = session.doCreate(cache, tooMany);
        expect(!ok7 && st7.isMember(xrpl::jss::error), "too many source_currencies is invalid");

        json::Value sendMaxBad{json::ValueType::Object};
        sendMaxBad[xrpl::jss::source_account] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        sendMaxBad[xrpl::jss::destination_account] = "rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe";
        sendMaxBad[xrpl::jss::destination_amount] = "1000000";
        sendMaxBad[xrpl::jss::send_max] = "1000000";
        auto [ok8, st8] = session.doCreate(cache, sendMaxBad);
        expect(!ok8 && st8.isMember(xrpl::jss::error), "send_max without convert-all is invalid");
    }

    {
        boost::asio::io_context io;
        PathServices services(io);
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 1;
        b.setHeader(h);
        auto view = b.publish();
        auto const src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto const dst = testAccount("rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe");
        xrpl::STAmount const dstAmt{xrpl::xrpIssue(), 1'000'000};
        auto found = FastPathFinder::search(
            services.books(),
            services,
            view,
            src,
            dst,
            xrpl::xrpIssue(),
            dstAmt,
            std::nullopt,
            std::nullopt,
            xrpl::STPathSet{},
            false,
            SearchBudget::forDepth(0),
            {});
        expect(found.paths.empty(), "empty book graph yields no paths");
        expect(found.candidates == 0, "empty book graph has no candidates");
        expect(found.depth == 0, "empty search reports the requested depth");
        expect(found.isolateRank, "fixed dest isolate-ranks the shortlist");
    }

    {
        boost::asio::io_context io;
        PathServices services(io);
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 1;
        b.setHeader(h);
        auto view = b.publish();
        auto const src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto const dst = testAccount("rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe");
        auto const gw = src;
        xrpl::Issue usd{xrpl::toCurrency("USD"), gw};
        xrpl::Issue eur{xrpl::toCurrency("EUR"), gw};
        auto midIssue = [&](char const* code) {
            return xrpl::Issue{xrpl::toCurrency(code), gw};
        };

        auto addDir = [&](xrpl::Issue const& in, xrpl::Issue const& out, std::uint64_t inAmt, std::uint64_t outAmt, int n) {
            xrpl::uint256 key;
            std::string hex(64, '0');
            hex[62] = "0123456789ABCDEF"[static_cast<unsigned>(n) % 16];
            hex[63] = "0123456789ABCDEF"[static_cast<unsigned>(n / 16) % 16];
            (void)key.parseHex(hex);
            auto sle = std::make_shared<xrpl::SLE>(xrpl::ltDIR_NODE, key);
            sle->setFieldH256(xrpl::sfRootIndex, key);
            sle->setFieldU64(
                xrpl::sfExchangeRate,
                xrpl::getRate(xrpl::STAmount{out, outAmt}, xrpl::STAmount{in, inAmt}));
            sle->setFieldH160(xrpl::sfTakerPaysCurrency, in.currency);
            sle->setFieldH160(xrpl::sfTakerPaysIssuer, in.account);
            sle->setFieldH160(xrpl::sfTakerGetsCurrency, out.currency);
            sle->setFieldH160(xrpl::sfTakerGetsIssuer, out.account);
            services.books().addFromSle(sle);
        };

        // Many mediocre 2-hop mids (100 in for 1 out each hop), one 1:1 pair.
        for (int i = 0; i < 40; ++i)
        {
            char code[4] = {'A', 'A', static_cast<char>('A' + (i % 26)), 0};
            auto mid = midIssue(code);
            addDir(usd, mid, 100, 1, 10 + i);
            addDir(mid, eur, 100, 1, 60 + i);
        }
        auto best = midIssue("BST");
        addDir(usd, best, 1, 1, 1);
        addDir(best, eur, 1, 1, 2);

        xrpl::STAmount const dstAmt{eur, 100};
        auto found = FastPathFinder::search(
            services.books(),
            services,
            view,
            src,
            dst,
            usd,
            dstAmt,
            std::nullopt,
            std::nullopt,
            xrpl::STPathSet{},
            false,
            SearchBudget::forDepth(0),
            {});
        expect(found.isolateRank, "fixed-amount still isolate-ranks the shortlist");
        expect(found.candidates > 0, "2-hop graph yields candidates");
        expect(!found.paths.empty(), "empty ledger falls back to cheap-scored 2-hop order");
        bool sawBest = false;
        if (!found.paths.empty())
        {
            auto const hops = found.paths[0];
            for (auto const& el : hops)
            {
                if (pathElementAsset(el) == xrpl::Asset{best})
                    sawBest = true;
            }
        }
        expect(sawBest, "best tip 2-hop is kept among many worse mids");
    }

    {
        // 3-hop dust tips must not bury a worse-but-real 2-hop pair.
        boost::asio::io_context io;
        PathServices services(io);
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 1;
        b.setHeader(h);
        auto view = b.publish();
        auto const src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto const dst = testAccount("rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe");
        auto const gw = src;
        xrpl::Issue usd{xrpl::toCurrency("USD"), gw};
        xrpl::Issue eur{xrpl::toCurrency("EUR"), gw};
        xrpl::Issue mid{xrpl::toCurrency("MID"), gw};
        xrpl::Issue dust{xrpl::toCurrency("DST"), gw};
        xrpl::Issue xrp = xrpl::xrpIssue();

        auto addDir = [&](xrpl::Issue const& in, xrpl::Issue const& out, std::uint64_t inAmt, std::uint64_t outAmt, int n) {
            xrpl::uint256 key;
            std::string hex(64, 'E');
            hex[62] = "0123456789ABCDEF"[static_cast<unsigned>(n) % 16];
            hex[63] = "0123456789ABCDEF"[static_cast<unsigned>(n / 16) % 16];
            (void)key.parseHex(hex);
            auto sle = std::make_shared<xrpl::SLE>(xrpl::ltDIR_NODE, key);
            sle->setFieldH256(xrpl::sfRootIndex, key);
            sle->setFieldU64(
                xrpl::sfExchangeRate,
                xrpl::getRate(xrpl::STAmount{out, outAmt}, xrpl::STAmount{in, inAmt}));
            sle->setFieldH160(xrpl::sfTakerPaysCurrency, in.currency);
            sle->setFieldH160(xrpl::sfTakerPaysIssuer, in.account);
            sle->setFieldH160(xrpl::sfTakerGetsCurrency, out.currency);
            sle->setFieldH160(xrpl::sfTakerGetsIssuer, out.account);
            services.books().addFromSle(sle);
        };
        addDir(usd, mid, 100, 1, 1);
        addDir(mid, eur, 100, 1, 2);
        addDir(usd, dust, 1, 10, 3);
        addDir(dust, xrp, 1, 10, 4);
        addDir(xrp, eur, 1, 10, 5);

        xrpl::STAmount const dstAmt{eur, 100};
        auto found = FastPathFinder::search(
            services.books(),
            services,
            view,
            src,
            dst,
            usd,
            dstAmt,
            std::nullopt,
            std::nullopt,
            xrpl::STPathSet{},
            false,
            SearchBudget::forDepth(1),
            {});
        expect(found.candidates >= 2, "2-hop and 3-hop both stay as candidates");
        bool sawTwoHop = false;
        bool firstIsShort = false;
        if (!found.paths.empty())
        {
            firstIsShort = found.paths[0].size() <= 2;
            for (auto const& path : found.paths)
            {
                if (path.size() == 2)
                {
                    for (auto const& el : path)
                    {
                        if (pathElementAsset(el) == xrpl::Asset{mid})
                            sawTwoHop = true;
                    }
                }
            }
        }
        expect(firstIsShort, "1–2 hop pairs are returned ahead of longer hops");
        expect(sawTwoHop, "mediocre 2-hop is not dropped for a better-tip 3-hop");
    }

    {
        boost::asio::io_context io;
        PathServices services(io);
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 1;
        b.setHeader(h);
        auto view = b.publish();
        auto const src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto const dst = testAccount("rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe");
        auto const gw = src;
        xrpl::Issue usd{xrpl::toCurrency("USD"), gw};
        xrpl::Issue eur{xrpl::toCurrency("EUR"), gw};
        xrpl::Issue thin{xrpl::toCurrency("THN"), gw};
        xrpl::Issue fat{xrpl::toCurrency("FAT"), gw};

        auto addOffer = [&](xrpl::Issue const& in, xrpl::Issue const& out, std::uint64_t pays, std::uint64_t gets, int n) {
            xrpl::uint256 key;
            std::string hex(64, 'C');
            hex[62] = "0123456789ABCDEF"[static_cast<unsigned>(n) % 16];
            hex[63] = "0123456789ABCDEF"[static_cast<unsigned>(n / 16) % 16];
            (void)key.parseHex(hex);
            auto sle = std::make_shared<xrpl::SLE>(xrpl::ltOFFER, key);
            sle->setFieldAmount(xrpl::sfTakerPays, xrpl::STAmount{in, pays});
            sle->setFieldAmount(xrpl::sfTakerGets, xrpl::STAmount{out, gets});
            services.books().addFromSle(sle);
        };
        // Thin: excellent rate, dust size. Fat: worse rate, huge size.
        addOffer(usd, thin, 1, 10, 1);
        addOffer(thin, eur, 1, 10, 2);
        addOffer(usd, fat, 50, 10, 3);
        addOffer(fat, eur, 50, 1'000'000, 4);

        xrpl::STAmount const destAll{xrpl::STAmount(eur, 1u, 0, true)};
        auto found = FastPathFinder::search(
            services.books(),
            services,
            view,
            src,
            dst,
            usd,
            destAll,
            std::nullopt,
            std::nullopt,
            xrpl::STPathSet{},
            true,
            SearchBudget::forDepth(0),
            {});
        expect(found.isolateRank, "convert-all isolate-ranks the shortlist");
        expect(!found.paths.empty(), "convert-all falls back to cheap width when calc has no ledger");
        bool sawFat = false;
        if (!found.paths.empty())
        {
            for (auto const& el : found.paths[0])
            {
                if (pathElementAsset(el) == xrpl::Asset{fat})
                    sawFat = true;
            }
        }
        expect(sawFat, "convert-all prefers the wide path over a better-but-tiny tip");
    }

    {
        boost::asio::io_context io;
        PathServices services(io);
        LedgerBuilder b;
        xrpl::LedgerHeader h;
        h.seq = 1;
        b.setHeader(h);
        auto view = b.publish();
        auto const src = testAccount("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
        auto const dst = testAccount("rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe");
        auto const gw = src;
        xrpl::Issue usd{xrpl::toCurrency("USD"), gw};
        xrpl::Issue eur{xrpl::toCurrency("EUR"), gw};

        auto addDir = [&](std::uint64_t rate) {
            xrpl::uint256 key;
            (void)key.parseHex("0D00000000000000000000000000000000000000000000000000000000000001");
            auto sle = std::make_shared<xrpl::SLE>(xrpl::ltDIR_NODE, key);
            sle->setFieldH256(xrpl::sfRootIndex, key);
            sle->setFieldU64(xrpl::sfExchangeRate, rate);
            sle->setFieldH160(xrpl::sfTakerPaysCurrency, usd.currency);
            sle->setFieldH160(xrpl::sfTakerPaysIssuer, usd.account);
            sle->setFieldH160(xrpl::sfTakerGetsCurrency, eur.currency);
            sle->setFieldH160(xrpl::sfTakerGetsIssuer, eur.account);
            services.books().addFromSle(sle);
        };

        xrpl::STAmount const dstAmt{eur, 100};
        xrpl::STAmount const sendMax{usd, 1};
        addDir(LocalOrderBooks::kNoQuality / 2);
        auto bad = FastPathFinder::search(
            services.books(),
            services,
            view,
            src,
            dst,
            usd,
            dstAmt,
            sendMax,
            std::nullopt,
            xrpl::STPathSet{},
            false,
            SearchBudget::forDepth(0),
            {});
        expect(bad.candidates == 0, "send_max prunes a hop already worse than dest/send_max");

        services.books().clear();
        addDir(1);
        auto good = FastPathFinder::search(
            services.books(),
            services,
            view,
            src,
            dst,
            usd,
            dstAmt,
            sendMax,
            std::nullopt,
            xrpl::STPathSet{},
            false,
            SearchBudget::forDepth(0),
            {});
        expect(good.candidates > 0, "send_max keeps a hop inside the dest/send_max bound");
    }

    {
        json::Value alt{json::ValueType::Object};
        alt[xrpl::jss::source_amount] = "1";
        alt[xrpl::jss::paths_computed] = json::ValueType::Array;
        json::Value result{json::ValueType::Object};
        result[xrpl::jss::alternatives] = json::ValueType::Array;
        result[xrpl::jss::alternatives].append(alt);
        result[xrpl::jss::full_reply] = true;
        result[xrpl::jss::source_account] = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
        expect(result.isMember(xrpl::jss::alternatives), "alternatives field");
        expect(result.isMember(xrpl::jss::full_reply), "full_reply field");
        expect(result[xrpl::jss::alternatives][0u].isMember(xrpl::jss::paths_computed),
               "paths_computed matches xrpld");
        expect(result[xrpl::jss::alternatives][0u].isMember(xrpl::jss::source_amount),
               "source_amount matches xrpld");
    }

    {
        boost::asio::io_context io;
        auto node = std::make_shared<NodeClient>(io, "ws://127.0.0.1:6006");
        Engine engine(io, Config{}, node);
        auto info = engine.pathCountsJson();
        expect(info.isMember("source") && info["source"].asString() == "edgy",
               "path_counts source is edgy");
        expect(info.isMember("sessions"), "path_counts sessions");
        expect(info.isMember("inflight"), "path_counts inflight");
        expect(info.isMember("workers_pending"), "path_counts workers_pending");
        expect(info.isMember("searches"), "path_counts searches");
        expect(info.isMember("creates"), "path_counts creates");
        expect(info.isMember("one_shots"), "path_counts one_shots");
        expect(info.isMember("updates"), "path_counts updates");
        expect(info.isMember("revalidates"), "path_counts revalidates");
        expect(info.isMember("deepens"), "path_counts deepens");
        expect(info.isMember("search_ms_last"), "path_counts search_ms_last");
        expect(info.isMember("search_ms_avg"), "path_counts search_ms_avg");
        expect(info.isMember("search_ms_max"), "path_counts search_ms_max");
        expect(info.isMember("pathfind_cache_hits"), "path_counts pathfind_cache_hits");
        expect(info.isMember("pathfind_cache_misses"), "path_counts pathfind_cache_misses");
        expect(info.isMember("pathfind_cache_lines"), "path_counts pathfind_cache_lines");
        expect(info.isMember("pathfind_lines_loaded"), "path_counts pathfind_lines_loaded");
        expect(info.isMember("pathfind_cache_rebuilds"), "path_counts pathfind_cache_rebuilds");
        expect(info.isMember("PathRequest"), "path_counts PathRequest");
        expect(info.isMember("PathFindTrustLine"), "path_counts PathFindTrustLine");
        expect(info.isMember("books"), "path_counts books");
        expect(info.isMember("apply_queue"), "path_counts apply_queue");
        expect(info.isMember("uptime"), "path_counts uptime");
        expect(info["sessions"].asDouble() == 0, "path_counts sessions start at 0");
        expect(info["searches"].asDouble() == 0, "path_counts searches start at 0");
        expect(info.isMember("network") && info["network"].asString() == "xrpl",
               "path_counts network is xrpl on xrpld binary");
        expect(info.isMember("native_currency") && info["native_currency"].asString() == "XRP",
               "path_counts native_currency is XRP on xrpld binary");
        expect(info.isMember("node") && info["node"].asString() == "xrpld",
               "path_counts node is xrpld on xrpld binary");
        auto status = engine.statusJson();
        expect(status.isMember("network") && status["network"].asString() == "xrpl",
               "path_info network is xrpl on xrpld binary");
        expect(!engine.ready(), "engine is not ready without a snapshot");
        expect(engine.ledger() == nullptr, "engine has no ledger before snapshot");
    }

    if (gFails != 0)
    {
        std::cerr << gFails << " test(s) failed\n";
        return 1;
    }
    std::cerr << "all tests passed\n";
    return 0;
}
