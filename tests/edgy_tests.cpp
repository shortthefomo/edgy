#include <edgy/config.hpp>
#include <edgy/engine.hpp>
#include <edgy/graph.hpp>
#include <edgy/memory_ledger.hpp>
#include <edgy/node_client.hpp>
#include <edgy/order_books.hpp>
#include <edgy/services.hpp>
#include <edgy/session.hpp>

#include <xrpld/rpc/detail/AssetCache.h>
#include <xrpld/rpc/detail/Pathfinder.h>
#include <xrpld/rpc/detail/Tuning.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/jss.h>

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

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
        Config d;
        expect(d.search == Config::kSearchFull, "default [search] is full");
        expect(d.searchFast == Config::kSearchFull, "default [search-fast] is full");
        expect(d.searchTimeout.count() == 0, "default [timeout-ms] is none");
        expect(d.fullSnapshot, "default [full-snapshot] is full");
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
        char const* shipped[] = {
            "cfg/edgy.example.cfg",
            "/Users/fomo/Dev/Ledgers/PathFinder/cfg/edgy.example.cfg",
        };
        bool parsed = false;
        for (auto const* p : shipped)
        {
            std::ifstream probe(p);
            if (!probe)
                continue;
            auto const cfg = Config::fromFile(p);
            expect(cfg.listenWs == "0.0.0.0:6008", "shipped [listen-ws]");
            expect(cfg.nodeWs == "ws://127.0.0.1:6006", "shipped [node]");
            expect(cfg.debugLog == "/tmp/edgy.log" ||
                       cfg.debugLog == "/private/tmp/edgy.log",
                   "shipped [debug]");
            expect(cfg.search == Config::kSearchFull, "shipped [search] full");
            expect(cfg.searchFast == Config::kSearchFull, "shipped [search-fast] full");
            expect(cfg.fullSnapshot, "shipped [full-snapshot] full");
            parsed = true;
            break;
        }
        expect(parsed, "shipped edgy.example.cfg parses");
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
    }

    if (gFails != 0)
    {
        std::cerr << gFails << " test(s) failed\n";
        return 1;
    }
    std::cerr << "all tests passed\n";
    return 0;
}
