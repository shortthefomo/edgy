#include <edgy/engine.hpp>

#include <edgy/thread_pool.hpp>

#include <xrpld/rpc/detail/Pathfinder.h>
#include <xrpld/rpc/detail/Tuning.h>

#include <xrpl/basics/Log.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxMeta.h>
#include <xrpl/protocol/jss.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace edgy {
namespace {

xrpl::SLE::pointer
sleFromBinary(std::string const& dataHex, std::string const& indexHex)
{
    auto const blob = xrpl::strUnHex(dataHex);
    if (!blob)
        return nullptr;
    xrpl::uint256 key;
    if (!key.parseHex(indexHex))
        return nullptr;
    try
    {
        xrpl::SerialIter sit(xrpl::makeSlice(*blob));
        return std::make_shared<xrpl::SLE>(sit, key);
    }
    catch (...)
    {
        return nullptr;
    }
}

bool
offerComplete(xrpl::SLE const& sle)
{
    if (sle.getType() == xrpl::ltAMM)
    {
        return sle.isFieldPresent(xrpl::sfLPTokenBalance) &&
            sle.isFieldPresent(xrpl::sfAsset) && sle.isFieldPresent(xrpl::sfAsset2) &&
            sle.getFieldAmount(xrpl::sfLPTokenBalance).signum() != 0;
    }
    return sle.getType() != xrpl::ltOFFER ||
        (sle.isFieldPresent(xrpl::sfTakerPays) && sle.isFieldPresent(xrpl::sfTakerGets) &&
         sle.isFieldPresent(xrpl::sfBookDirectory) && sle.isFieldPresent(xrpl::sfAccount) &&
         sle.getFieldAmount(xrpl::sfTakerPays).signum() != 0 &&
         sle.getFieldAmount(xrpl::sfTakerGets).signum() != 0);
}

xrpl::SLE::pointer
sleFromMetaNode(xrpl::STObject const& node, xrpl::STObject const& inner, xrpl::uint256 const& key)
{
    xrpl::STObject fields{inner};
    if (!fields.isFieldPresent(xrpl::sfLedgerEntryType) &&
        node.isFieldPresent(xrpl::sfLedgerEntryType))
    {
        fields.setFieldU16(
            xrpl::sfLedgerEntryType, node.getFieldU16(xrpl::sfLedgerEntryType));
    }
    return std::make_shared<xrpl::SLE>(fields, key);
}

enum class MetaApply : std::uint8_t
{
    None,
    Created,
    Modified,
    Deleted,
    Incomplete,
};

MetaApply
applyMetaNode(LedgerBuilder& builder, LocalOrderBooks& books, xrpl::STObject const& node)
{
    try
    {
        auto const& fname = node.getFName();
        xrpl::SField const* innerField = nullptr;
        bool const deleted = fname == xrpl::sfDeletedNode;
        if (fname == xrpl::sfCreatedNode)
            innerField = &xrpl::sfNewFields;
        else if (fname == xrpl::sfModifiedNode)
            innerField = &xrpl::sfFinalFields;
        else if (!deleted)
            return MetaApply::None;

        if (!node.isFieldPresent(xrpl::sfLedgerIndex))
            return MetaApply::None;
        auto const key = node.getFieldH256(xrpl::sfLedgerIndex);
        if (deleted)
        {
            if (node.isFieldPresent(xrpl::sfFinalFields))
            {
                auto const& inner =
                    dynamic_cast<xrpl::STObject const&>(node.peekAtField(xrpl::sfFinalFields));
                books.removeFromSle(sleFromMetaNode(node, inner, key));
            }
            builder.erase(key);
            return MetaApply::Deleted;
        }
        if (!innerField || !node.isFieldPresent(*innerField))
            return MetaApply::None;
        auto const& inner = dynamic_cast<xrpl::STObject const&>(node.peekAtField(*innerField));
        auto sle = sleFromMetaNode(node, inner, key);
        if (!offerComplete(*sle))
        {
            books.removeFromSle(sle);
            builder.erase(key);
            return MetaApply::Incomplete;
        }
        books.addFromSle(sle);
        builder.upsert(std::move(sle));
        return fname == xrpl::sfCreatedNode ? MetaApply::Created : MetaApply::Modified;
    }
    catch (std::exception const& ex)
    {
        std::cerr << "applyMetaNode: " << ex.what() << '\n';
        return MetaApply::None;
    }
}

std::uint32_t
msgLedgerIndex(json::Value const& msg)
{
    auto const take = [](json::Value const& v) -> std::uint32_t {
        if (v.isIntegral())
            return v.asUInt();
        if (v.isString())
        {
            try
            {
                return static_cast<std::uint32_t>(std::stoul(v.asString()));
            }
            catch (...)
            {
                return 0;
            }
        }
        return 0;
    };
    if (msg.isMember(xrpl::jss::ledger_index))
        return take(msg[xrpl::jss::ledger_index]);
    return 0;
}

void
fillHeaderFromLedgerJson(xrpl::LedgerHeader& header, json::Value const& ledger)
{
    if (ledger.isMember(xrpl::jss::ledger_index))
    {
        if (ledger[xrpl::jss::ledger_index].isIntegral())
            header.seq = ledger[xrpl::jss::ledger_index].asUInt();
        else if (ledger[xrpl::jss::ledger_index].isString())
            header.seq = static_cast<xrpl::LedgerIndex>(
                std::stoul(ledger[xrpl::jss::ledger_index].asString()));
    }
    if (ledger.isMember(xrpl::jss::ledger_hash) && ledger[xrpl::jss::ledger_hash].isString())
        (void)header.hash.parseHex(ledger[xrpl::jss::ledger_hash].asString());
    if (ledger.isMember(xrpl::jss::parent_hash) && ledger[xrpl::jss::parent_hash].isString())
        (void)header.parentHash.parseHex(ledger[xrpl::jss::parent_hash].asString());
    if (ledger.isMember(xrpl::jss::account_hash) && ledger[xrpl::jss::account_hash].isString())
        (void)header.accountHash.parseHex(ledger[xrpl::jss::account_hash].asString());
    if (ledger.isMember(xrpl::jss::transaction_hash) &&
        ledger[xrpl::jss::transaction_hash].isString())
        (void)header.txHash.parseHex(ledger[xrpl::jss::transaction_hash].asString());
    if (ledger.isMember("total_coins") && ledger["total_coins"].isString())
        header.drops = xrpl::XRPAmount{std::stoll(ledger["total_coins"].asString())};
    header.validated = true;
    header.accepted = true;
}

std::uint32_t
msgTxnCount(json::Value const& msg)
{
    if (!msg.isMember(xrpl::jss::txn_count))
        return 0;
    auto const& v = msg[xrpl::jss::txn_count];
    if (v.isIntegral())
        return v.asUInt();
    if (v.isString())
    {
        try
        {
            return static_cast<std::uint32_t>(std::stoul(v.asString()));
        }
        catch (...)
        {
            return 0;
        }
    }
    return 0;
}

std::string
shortHash(xrpl::uint256 const& hash)
{
    auto const s = to_string(hash);
    return s.size() > 12 ? s.substr(0, 12) : s;
}

}  // namespace

Engine::Engine(boost::asio::io_context& io, Config cfg, std::shared_ptr<NodeClient> node)
    : cfg_(std::move(cfg))
    , node_(std::move(node))
    , services_(io)
    , pool_(std::make_unique<ThreadPool>(static_cast<std::size_t>(cfg_.workers)))
{
    xrpl::Pathfinder::initPathTable();
}

Engine::~Engine()
{
    stop();
}

void
Engine::start()
{
    node_->onLedger([this](json::Value const& msg) { enqueueApply(true, msg); });
    node_->onTransaction([this](json::Value const& msg) { enqueueApply(false, msg); });
    node_->onDisconnect([this](std::string const& why) {
        std::cerr << "upstream disconnected: " << why << '\n';
        ready_.store(false);
        needResync_.store(true);
        applyCv_.notify_all();
    });
    applyThread_ = std::thread([this] { applyLoop(); });
    syncThread_ = std::thread([this] { syncLoop(); });
}

void
Engine::stop()
{
    stop_.store(true);
    if (pool_)
        pool_->shutdown();
    services_.stop();
    applyCv_.notify_all();
    if (syncThread_.joinable())
        syncThread_.join();
    if (applyThread_.joinable())
        applyThread_.join();
}

bool
Engine::ready() const
{
    return ready_.load();
}

std::shared_ptr<xrpl::ReadView const>
Engine::ledger() const
{
    std::lock_guard lock(stateMutex_);
    return published_;
}

json::Value
Engine::statusJson() const
{
    json::Value j{json::ValueType::Object};
    std::lock_guard lock(stateMutex_);
    j["server_state"] = ready_.load() ? "full" : "syncing";
    j["load_factor"] = 1;
    if (published_)
    {
        auto const seq = published_->seq();
        auto const first = firstSeq_.load() ? firstSeq_.load() : seq;
        j[xrpl::jss::ledger_index] = seq;
        j[xrpl::jss::ledger_hash] = to_string(published_->header().hash);
        j[xrpl::jss::complete_ledgers] = std::to_string(first) + "-" + std::to_string(seq);
        json::Value vl{json::ValueType::Object};
        vl[xrpl::jss::seq] = seq;
        vl[xrpl::jss::hash] = to_string(published_->header().hash);
        vl["base_fee_xrp"] = 0.00001;
        vl["reserve_base_xrp"] = static_cast<double>(published_->fees().reserve.drops()) / 1'000'000.0;
        vl["reserve_inc_xrp"] = static_cast<double>(published_->fees().increment.drops()) / 1'000'000.0;
        j["validated_ledger"] = std::move(vl);
        j["objects"] = static_cast<std::uint32_t>(published_->size());
        j["overlay"] = static_cast<std::uint32_t>(published_->overlaySize());
    }
    else
    {
        j[xrpl::jss::complete_ledgers] = "empty";
        j["objects"] = static_cast<std::uint32_t>(objects_.load());
    }
    j["workers"] = cfg_.workers;
    j["update_ms"] = static_cast<std::uint32_t>(cfg_.midCloseDelay.count());
    j["search"] = cfg_.search;
    j["search_fast"] = cfg_.searchFast;
    j["timeout_ms"] = static_cast<std::uint32_t>(cfg_.searchTimeout.count());
    j["full_snapshot"] = cfg_.fullSnapshot;
    j["searches"] = static_cast<std::uint32_t>(searches_.load());
    if (cache_)
    {
        j["pathfind_cache_lines"] = static_cast<std::uint32_t>(cache_->totalLineCount());
        j["pathfind_cache_hits"] = static_cast<std::uint32_t>(cache_->cacheHits());
        j["pathfind_cache_misses"] = static_cast<std::uint32_t>(cache_->cacheMisses());
    }
    return j;
}

void
Engine::noteSearchMs(std::uint64_t ms)
{
    searchMsLast_.store(ms, std::memory_order_relaxed);
    searchMsSum_.fetch_add(ms, std::memory_order_relaxed);
    auto prev = searchMsMax_.load(std::memory_order_relaxed);
    while (ms > prev &&
           !searchMsMax_.compare_exchange_weak(prev, ms, std::memory_order_relaxed))
    {
    }
}

json::Value
Engine::pathCountsJson() const
{
    json::Value j{json::ValueType::Object};
    auto setU64 = [](json::Value& obj, char const* key, std::uint64_t v) {
        obj[key] = static_cast<double>(v);
    };

    auto const up = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startedAt_);
    auto secs = up.count();
    std::string uptime;
    auto addUnit = [&](char const* name, long long unit) {
        auto n = secs / unit;
        if (n <= 0)
            return;
        secs %= unit;
        if (!uptime.empty())
            uptime += ", ";
        uptime += std::to_string(n);
        uptime += " ";
        uptime += name;
        if (n != 1)
            uptime += "s";
    };
    addUnit("day", 86400);
    addUnit("hour", 3600);
    addUnit("minute", 60);
    addUnit("second", 1);
    if (uptime.empty())
        uptime = "0 seconds";

    std::size_t sessions = 0;
    std::size_t inflight = 0;
    int depthMax = 0;
    {
        std::lock_guard lock(subMutex_);
        sessions = subs_.size();
        for (auto const& [id, sub] : subs_)
        {
            if (!sub.session)
                continue;
            if (sub.session->updating())
                ++inflight;
            depthMax = std::max(depthMax, sub.session->lastDepth());
        }
    }

    std::size_t applyQ = 0;
    {
        std::lock_guard lock(applyMutex_);
        applyQ = applyQueue_.size();
    }

    j["source"] = "edgy";
    j["server_state"] = ready_.load() ? "full" : "syncing";
    j["uptime"] = uptime;
    j["load_factor"] = 1;
    setU64(j, "sessions", sessions);
    setU64(j, "inflight", inflight);
    setU64(j, "PathRequest", sessions);
    j["search_depth"] = depthMax;
    j["workers"] = cfg_.workers;
    setU64(j, "workers_pending", pool_ ? pool_->pending() : 0);
    j["net_threads"] = cfg_.netThreads;
    j["update_ms"] = static_cast<std::uint32_t>(cfg_.midCloseDelay.count());
    j["search"] = cfg_.search;
    j["search_fast"] = cfg_.searchFast;
    j["timeout_ms"] = static_cast<std::uint32_t>(cfg_.searchTimeout.count());
    j["full_snapshot"] = cfg_.fullSnapshot;
    setU64(j, "apply_queue", applyQ);
    setU64(j, "books", services_.books().bookCount());
    setU64(j, "searches", searches_.load(std::memory_order_relaxed));
    setU64(j, "creates", creates_.load(std::memory_order_relaxed));
    setU64(j, "one_shots", oneShots_.load(std::memory_order_relaxed));
    setU64(j, "updates", updates_.load(std::memory_order_relaxed));
    setU64(j, "revalidates", revalidates_.load(std::memory_order_relaxed));
    setU64(j, "deepens", deepens_.load(std::memory_order_relaxed));
    setU64(j, "errors", errors_.load(std::memory_order_relaxed));
    auto const nSearch = searches_.load(std::memory_order_relaxed);
    auto const sumMs = searchMsSum_.load(std::memory_order_relaxed);
    setU64(j, "search_ms_last", searchMsLast_.load(std::memory_order_relaxed));
    setU64(j, "search_ms_max", searchMsMax_.load(std::memory_order_relaxed));
    setU64(j, "search_ms_avg", nSearch ? sumMs / nSearch : 0);

    {
        std::lock_guard lock(stateMutex_);
        if (published_)
        {
            auto const seq = published_->seq();
            auto const first = firstSeq_.load() ? firstSeq_.load() : seq;
            j[xrpl::jss::ledger_index] = seq;
            j[xrpl::jss::complete_ledgers] = std::to_string(first) + "-" + std::to_string(seq);
            setU64(j, "objects", published_->size());
            setU64(j, "overlay", published_->overlaySize());
        }
        else
        {
            j[xrpl::jss::complete_ledgers] = "empty";
            setU64(j, "objects", objects_.load());
            setU64(j, "overlay", 0);
        }
        if (cache_)
        {
            auto const lines = cache_->totalLineCount();
            setU64(j, "pathfind_cache_hits", cache_->cacheHits());
            setU64(j, "pathfind_cache_misses", cache_->cacheMisses());
            setU64(j, "pathfind_lines_loaded", cache_->linesLoaded());
            setU64(j, "pathfind_cache_advances", cache_->ledgerAdvances());
            setU64(j, "pathfind_cache_rebuilds", cache_->ledgerAdvances());
            setU64(j, "pathfind_cache_lines", lines);
            setU64(j, "PathFindTrustLine", lines);
            setU64(j, "pathfind_cache_budget", cfg_.maxTotalLines);
            j["pathfind_cache_over_budget"] = cache_->overBudget();
        }
        else
        {
            setU64(j, "pathfind_cache_hits", 0);
            setU64(j, "pathfind_cache_misses", 0);
            setU64(j, "pathfind_lines_loaded", 0);
            setU64(j, "pathfind_cache_advances", 0);
            setU64(j, "pathfind_cache_rebuilds", 0);
            setU64(j, "pathfind_cache_lines", 0);
            setU64(j, "PathFindTrustLine", 0);
            setU64(j, "pathfind_cache_budget", cfg_.maxTotalLines);
            j["pathfind_cache_over_budget"] = false;
        }
    }
    return j;
}

void
Engine::enqueueApply(bool ledgerClosed, json::Value msg)
{
    {
        std::lock_guard lock(applyMutex_);
        if (applyQueue_.size() > 20'000)
        {
            while (!applyQueue_.empty())
                applyQueue_.pop();
            ready_.store(false);
            needResync_.store(true);
            std::cerr << "apply queue overflow; will resnapshot\n";
            applyCv_.notify_all();
            return;
        }
        applyQueue_.push(ApplyItem{ledgerClosed, std::move(msg)});
    }
    applyCv_.notify_one();
}

void
Engine::waitApplyIdle()
{
    std::unique_lock lock(applyMutex_);
    applyCv_.notify_all();
    applyCv_.wait(lock, [this] { return stop_.load() || applyIdle_.load(); });
}

void
Engine::forgetConnSessions(int connId, std::shared_ptr<xrpl::AssetCache> const& cache)
{
    auto range = connToSession_.equal_range(connId);
    for (auto it = range.first; it != range.second; ++it)
    {
        auto sit = subs_.find(it->second);
        if (sit == subs_.end())
            continue;
        sit->second.session->doClose();
        if (cache)
        {
            cache->releaseSession(sit->second.session->id());
            cache->forgetSession(sit->second.session->id());
        }
        subs_.erase(sit);
    }
    connToSession_.erase(connId);
}

void
Engine::applyLoop()
{
    lastTick_ = std::chrono::steady_clock::now();
    for (;;)
    {
        std::vector<ApplyItem> batch;
        {
            std::unique_lock lock(applyMutex_);
            applyIdle_.store(true);
            applyCv_.notify_all();
            applyCv_.wait_for(lock, cfg_.midCloseDelay, [this] {
                return stop_.load() || (ready_.load() && !applyQueue_.empty());
            });
            if (stop_.load() && applyQueue_.empty())
                return;
            if (!ready_.load() || applyQueue_.empty())
                continue;
            applyIdle_.store(false);
            while (!applyQueue_.empty())
            {
                batch.push_back(std::move(applyQueue_.front()));
                applyQueue_.pop();
                if (batch.size() >= 256)
                    break;
            }
        }
        try
        {
            for (auto const& item : batch)
            {
                if (item.ledgerClosed)
                    applyLedgerClosed(item.msg);
                else
                    applyTransaction(item.msg);
            }
        }
        catch (std::exception const& ex)
        {
            std::cerr << "apply error: " << ex.what() << '\n';
        }

        auto const now = std::chrono::steady_clock::now();
        if (!stop_.load() && ready_.load() && now - lastTick_ >= cfg_.midCloseDelay)
        {
            lastTick_ = now;
            try
            {
                midCloseTick();
            }
            catch (std::exception const& ex)
            {
                std::cerr << "mid-close tick error: " << ex.what() << '\n';
            }
        }
    }
}

void
Engine::syncLoop()
{
    auto j = services_.getJournal("Engine");
    bool subscribed = false;
    while (!stop_.load())
    {
        try
        {
            while (!stop_.load() && !node_->connected())
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            if (stop_.load())
                return;

            waitApplyIdle();
            if (stop_.load())
                return;

            std::cerr << "loading " << (cfg_.fullSnapshot ? "full" : "books")
                      << " ledger snapshot from " << cfg_.nodeWs << '\n';
            JLOG(j.info()) << "loading ledger snapshot from " << cfg_.nodeWs;
            loadSnapshot();
            if (!subscribed)
            {
                json::Value sub{json::ValueType::Object};
                sub[xrpl::jss::streams] = json::Value{json::ValueType::Array};
                sub[xrpl::jss::streams].append("ledger");
                sub[xrpl::jss::streams].append("transactions");
                sub[xrpl::jss::binary] = true;
                (void)node_->request("subscribe", sub, std::chrono::seconds{15});
                subscribed = true;
            }
            if (auto view = ledger(); view && firstSeq_.load() == 0)
                firstSeq_.store(view->seq());
            needResync_.store(false);
            ready_.store(true);
            applyCv_.notify_all();
            if (auto view = ledger())
            {
                std::cerr << "sync ready ledger " << view->seq() << " "
                          << shortHash(view->header().hash) << " objects=" << objects_.load()
                          << " books=" << services_.books().bookCount()
                          << "; following node closes\n";
            }
            else
            {
                std::cerr << "snapshot ready (" << objects_.load()
                          << " objects); serving local path_find\n";
            }
            JLOG(j.info()) << "snapshot ready; serving local path_find";

            while (!stop_.load())
            {
                {
                    std::unique_lock lock(applyMutex_);
                    applyCv_.wait_for(lock, std::chrono::seconds{1}, [this] {
                        return stop_.load() || needResync_.load() || !node_->connected();
                    });
                }
                if (stop_.load())
                    return;
                if (needResync_.load() || !node_->connected())
                {
                    ready_.store(false);
                    applyCv_.notify_all();
                    if (!node_->connected())
                    {
                        subscribed = false;
                        needResync_.store(true);
                    }
                    break;
                }
            }
            if (stop_.load())
                return;
            if (!node_->connected())
            {
                std::cerr << "upstream down; reconnecting for resnapshot\n";
                node_->stop();
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                if (!stop_.load())
                    node_->run();
            }
        }
        catch (std::exception const& ex)
        {
            ready_.store(false);
            subscribed = false;
            applyCv_.notify_all();
            if (stop_.load())
                return;
            std::cerr << "snapshot failed: " << ex.what() << " (retry in 3s)\n";
            JLOG(j.error()) << "snapshot failed: " << ex.what();
            std::this_thread::sleep_for(std::chrono::seconds{3});
            if (stop_.load())
                return;
            node_->stop();
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            if (!stop_.load())
                node_->run();
        }
    }
}

void
Engine::loadSnapshot()
{
    services_.books().clear();
    resetApplyStats();
    {
        std::lock_guard lock(stateMutex_);
        cache_.reset();
        published_.reset();
    }
    builder_.clear();
    builder_.reserve(25'000'000);
    json::Value ledgerReq{json::ValueType::Object};
    ledgerReq[xrpl::jss::ledger_index] = "validated";
    auto ledgerRes = node_->request("ledger", ledgerReq, std::chrono::minutes{2});
    if (ledgerRes.isMember(xrpl::jss::error))
        throw std::runtime_error(ledgerRes[xrpl::jss::error].asString());

    xrpl::LedgerHeader header;
    if (ledgerRes.isMember(xrpl::jss::ledger))
        fillHeaderFromLedgerJson(header, ledgerRes[xrpl::jss::ledger]);
    builder_.setHeader(header);
    if (firstSeq_.load() == 0)
        firstSeq_.store(header.seq);
    currentSeq_.store(header.seq);
    std::cerr << "snapshot ledger " << header.seq << " " << to_string(header.hash) << '\n';

    auto loadType = [&](std::optional<std::string> type, bool optionalType = false) {
        json::Value marker;
        std::uint64_t loaded = 0;
        int page = 0;
        auto const label = type ? *type : std::string("all");
        for (;;)
        {
            if (stop_.load())
                throw std::runtime_error("stopped");
            json::Value req{json::ValueType::Object};
            // String index: Clio/public hubs accept it; numeric can fail.
            req[xrpl::jss::ledger_index] = std::to_string(header.seq);
            req[xrpl::jss::binary] = true;
            req[xrpl::jss::limit] = 256;
            if (type)
                req[xrpl::jss::type] = *type;
            if (!marker.isNull())
                req[xrpl::jss::marker] = marker;
            ++page;
            auto const t0 = std::chrono::steady_clock::now();
            auto res = node_->request("ledger_data", req, std::chrono::minutes{2});
            auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            if (res.isMember(xrpl::jss::error))
            {
                auto const err = res[xrpl::jss::error].asString();
                if (optionalType)
                {
                    std::cerr << "  snapshot " << label << " skipped: " << err << '\n';
                    return;
                }
                throw std::runtime_error("ledger_data " + label + ": " + err);
            }
            int batch = 0;
            if (res.isMember("state") && res["state"].isArray())
            {
                for (auto const& item : res["state"])
                {
                    if (!item.isObject() || !item.isMember("data") || !item.isMember("index"))
                        continue;
                    auto const dataHex = item["data"].asString();
                    auto const indexHex = item["index"].asString();
                    xrpl::uint256 key;
                    auto const blob = xrpl::strUnHex(dataHex);
                    if (!blob || !key.parseHex(indexHex))
                        continue;
                    if (auto sle = sleFromBinary(dataHex, indexHex))
                    {
                        services_.books().addFromSle(sle);
                        builder_.upsertRaw(key, std::move(*blob), sle);
                    }
                    else
                    {
                        builder_.upsertRaw(key, std::move(*blob));
                    }
                    ++loaded;
                    ++batch;
                    objects_.store(builder_.size());
                }
            }
            else if (page == 1)
            {
                throw std::runtime_error(
                    "ledger_data returned no state[] (keys missing or public hub "
                    "rejected binary paging)");
            }
            std::cerr << "  snapshot " << label << " page " << page << ": +" << batch
                      << " (" << builder_.size() << " objects, " << ms << "ms)\n";
            if (!res.isMember(xrpl::jss::marker) || res[xrpl::jss::marker].isNull())
                break;
            marker = res[xrpl::jss::marker];
            if (batch == 0)
                throw std::runtime_error("ledger_data marker present but page was empty");
        }
    };

    if (cfg_.fullSnapshot)
    {
        loadType(std::nullopt);
    }
    else
    {
        // Path-relevant types only: books, AMMs, directories, accounts, lines.
        for (char const* type : {"offer", "amm", "directory", "account", "state"})
            loadType(std::string{type});
        // Fees/amendments are required for RippleCalc even on a books snapshot.
        // Some public hubs reject these type filters; those are best-effort.
        loadType(std::string{"fee"}, true);
        loadType(std::string{"amendments"}, true);
    }

    publishBuilder(false);
}

void
Engine::publishBuilder(bool rebuildBooks)
{
    auto view = builder_.publish();
    objects_.store(view->size());
    std::shared_ptr<xrpl::AssetCache> cache;
    bool created = false;
    {
        std::lock_guard lock(stateMutex_);
        published_ = view;
        if (!cache_)
        {
            cache_ = std::make_shared<xrpl::AssetCache>(
                view,
                services_.getJournal("AssetCache"),
                cfg_.maxTotalLines,
                cfg_.maxLinesPerAccount,
                cfg_.cacheReuseLedgers,
                cfg_.lineChunkSize);
            created = true;
        }
        cache = cache_;
    }
    if (rebuildBooks)
        services_.books().setup(view);
    if (cache && !created)
        cache->advanceLedger(view);
}

void
Engine::resetApplyStats()
{
    applyTxs_ = 0;
    applyNoMeta_ = 0;
    applyCreated_ = 0;
    applyDeleted_ = 0;
    applyModified_ = 0;
    applyIncomplete_ = 0;
}

void
Engine::logSync(char const* what, xrpl::LedgerHeader const& header, json::Value const& nodeMsg)
{
    std::size_t objects = builder_.size();
    std::size_t overlay = 0;
    {
        std::lock_guard lock(stateMutex_);
        if (published_)
        {
            objects = published_->size();
            overlay = published_->overlaySize();
        }
    }
    auto const books = services_.books().bookCount();
    auto const nodeSeq = msgLedgerIndex(nodeMsg);
    auto const nodeTxs = msgTxnCount(nodeMsg);
    bool const txOk = nodeTxs == 0 || (applyTxs_ + applyNoMeta_) == nodeTxs;
    bool const seqOk = nodeSeq == 0 || nodeSeq == header.seq;

    std::cerr << "sync " << what << " ledger " << header.seq << " " << shortHash(header.hash)
              << " objects=" << objects << " overlay=" << overlay << " books=" << books
              << " txs=" << applyTxs_;
    if (nodeTxs != 0)
        std::cerr << "/" << nodeTxs;
    if (applyNoMeta_ != 0)
        std::cerr << " no_meta=" << applyNoMeta_;
    std::cerr << " created=" << applyCreated_ << " deleted=" << applyDeleted_
              << " modified=" << applyModified_;
    if (applyIncomplete_ != 0)
        std::cerr << " incomplete=" << applyIncomplete_;
    if (seqOk && txOk)
        std::cerr << " inline with node";
    else
    {
        std::cerr << " WARNING";
        if (!seqOk)
            std::cerr << " seq!=" << nodeSeq;
        if (!txOk)
            std::cerr << " txs!=" << nodeTxs;
    }
    std::cerr << '\n';
}

void
Engine::applyLedgerClosed(json::Value const& msg)
{
    if (!ready_.load())
        return;
    auto const seq = msgLedgerIndex(msg);
    auto const prevSeq = currentSeq_.load();
    if (seq != 0 && seq <= prevSeq)
        return;
    if (seq != 0 && prevSeq != 0 && seq > prevSeq + 1)
    {
        std::cerr << "sync WARNING skipped ledgers " << (prevSeq + 1) << "-" << (seq - 1)
                  << " (have " << prevSeq << ", node closed " << seq
                  << "); objects may drift — resnapshot\n";
        needResync_.store(true);
        applyCv_.notify_all();
    }
    xrpl::LedgerHeader prev = builder_.header();
    xrpl::LedgerHeader header = prev;
    fillHeaderFromLedgerJson(header, msg);
    if (prev.seq != 0 && prev.hash != beast::kZero && header.parentHash != beast::kZero &&
        header.parentHash != prev.hash)
    {
        std::cerr << "sync WARNING parent " << shortHash(header.parentHash)
                  << " != local " << shortHash(prev.hash) << " at ledger " << header.seq
                  << "; objects not chained to the node\n";
        needResync_.store(true);
        applyCv_.notify_all();
    }
    builder_.setHeader(header);
    builder_.setOpen(false);
    currentSeq_.store(header.seq);
    publishBuilder(false);
    dirty_.store(false, std::memory_order_release);
    logSync("closed", header, msg);
    resetApplyStats();
    if (auto cache = cache_)
        cache->expandIncompleteLines();
    json::Value closed;
    {
        std::lock_guard lock(handlerMutex_);
        if (ledgerClosedHandler_)
            closed = ledgerClosedJson();
    }
    if (!closed.isNull())
    {
        std::function<void(json::Value)> handler;
        {
            std::lock_guard lock(handlerMutex_);
            handler = ledgerClosedHandler_;
        }
        if (handler)
            handler(std::move(closed));
    }
    // Reprice only. A full search of every live session on close
    // convoyed the worker pool at 50–100 sockets.
    notifySubscriptions(true);
}

void
Engine::applyTransaction(json::Value const& msg)
{
    if (!ready_.load())
        return;
    if (auto const seq = msgLedgerIndex(msg); seq != 0 && seq <= currentSeq_.load())
        return;
    if (msg.isMember(xrpl::jss::validated) && msg[xrpl::jss::validated].isBool() &&
        !msg[xrpl::jss::validated].asBool())
        return;
    builder_.setOpen(true);

    // Binary metadata only. JSON FinalFields applies were dropping offer
    // amounts / BookDirectory and left BookTip stepping the same empty
    // offer forever (Flow WRN flood).
    std::string hex;
    if (msg.isMember(xrpl::jss::meta) && msg[xrpl::jss::meta].isString())
        hex = msg[xrpl::jss::meta].asString();
    else if (msg.isMember("meta_blob") && msg["meta_blob"].isString())
        hex = msg["meta_blob"].asString();
    if (hex.empty())
    {
        ++applyNoMeta_;
        return;
    }
    auto const blob = xrpl::strUnHex(hex);
    if (!blob)
    {
        ++applyNoMeta_;
        return;
    }
    try
    {
        xrpl::TxMeta meta(xrpl::uint256{}, 0, *blob);
        bool any = false;
        for (auto const& node : meta.getNodes())
        {
            switch (applyMetaNode(builder_, services_.books(), node))
            {
                case MetaApply::Created:
                    ++applyCreated_;
                    any = true;
                    break;
                case MetaApply::Modified:
                    ++applyModified_;
                    any = true;
                    break;
                case MetaApply::Deleted:
                    ++applyDeleted_;
                    any = true;
                    break;
                case MetaApply::Incomplete:
                    ++applyIncomplete_;
                    any = true;
                    break;
                case MetaApply::None:
                    break;
            }
        }
        ++applyTxs_;
        if (any)
            dirty_.store(true, std::memory_order_release);
    }
    catch (std::exception const& ex)
    {
        ++applyNoMeta_;
        std::cerr << "applyTransaction: " << ex.what() << '\n';
    }
}

void
Engine::midCloseTick()
{
    if (pool_->pending() > pool_->size())
        return;
    bool live = false;
    {
        std::lock_guard lock(subMutex_);
        for (auto const& [id, sub] : subs_)
        {
            if (sub.session && !sub.session->closing())
            {
                live = true;
                break;
            }
        }
    }
    if (!live)
        return;
    if (dirty_.exchange(false, std::memory_order_acq_rel))
        publishBuilder(false);
    notifySubscriptions(true);
}

void
Engine::notifySubscriptions(bool revalidateOnly)
{
    if (stop_.load() || !pool_)
        return;

    std::vector<Sub> live;
    {
        std::lock_guard lock(subMutex_);
        live.reserve(subs_.size());
        for (auto const& [id, sub] : subs_)
        {
            if (sub.session && !sub.session->closing())
                live.push_back(sub);
        }
    }

    std::shared_ptr<xrpl::AssetCache> cache;
    {
        std::lock_guard lock(stateMutex_);
        cache = cache_;
    }
    if (!cache)
        return;

    int deepenLeft = 4;
    for (auto const& sub : live)
    {
        if (!sub.session->tryBeginUpdate())
            continue;
        bool reval = revalidateOnly;
        if (reval && sub.session->shouldDeepen() && deepenLeft > 0)
        {
            reval = false;
            --deepenLeft;
        }
        bool const deepen = !reval;
        try
        {
            pool_->submit([this, sub, cache, reval, deepen] {
                try
                {
                    if (!sub.session->closing())
                    {
                        searches_.fetch_add(1);
                        if (reval)
                            revalidates_.fetch_add(1);
                        else
                            updates_.fetch_add(1);
                        if (deepen)
                            deepens_.fetch_add(1);
                        auto const t0 = std::chrono::steady_clock::now();
                        auto update = sub.session->doUpdate(cache, false, reval);
                        noteSearchMs(static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count()));
                        update[xrpl::jss::type] = "path_find";
                        if (sub.push)
                            sub.push(std::move(update));
                    }
                }
                catch (...)
                {
                    errors_.fetch_add(1);
                }
                sub.session->endUpdate();
            });
        }
        catch (...)
        {
            errors_.fetch_add(1);
            sub.session->endUpdate();
        }
    }
}

json::Value
Engine::runRipplePathFind(json::Value const& params)
{
    if (!ready_.load())
        return xrpl::rpcError(xrpl::RpcNotSynced);

    std::shared_ptr<xrpl::AssetCache> cache;
    {
        std::lock_guard lock(stateMutex_);
        cache = cache_;
    }
    auto const id = nextId_.fetch_add(1);
    auto session = std::make_shared<PathSession>(
        services_, cfg_, id, true, services_.getJournal("PathSession"));
    searches_.fetch_add(1);
    oneShots_.fetch_add(1);
    auto const t0 = std::chrono::steady_clock::now();
    auto [valid, status] = session->doCreate(cache, params);
    if (!valid)
    {
        errors_.fetch_add(1);
        return status;
    }
    auto result = session->doUpdate(cache, false);
    noteSearchMs(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count()));
    return result;
}

json::Value
Engine::runPathFind(json::Value const& params, int connId, PushFn push)
{
    if (!ready_.load())
        return xrpl::rpcError(xrpl::RpcNotSynced);
    if (!params.isMember(xrpl::jss::subcommand) || !params[xrpl::jss::subcommand].isString())
        return xrpl::rpcError(xrpl::RpcInvalidParams);

    auto const sub = params[xrpl::jss::subcommand].asString();
    if (sub == "create")
    {
        std::shared_ptr<xrpl::AssetCache> cache;
        {
            std::lock_guard lock(stateMutex_);
            cache = cache_;
        }
        auto const id = nextId_.fetch_add(1);
        auto session = std::make_shared<PathSession>(
            services_, cfg_, id, false, services_.getJournal("PathSession"));
        searches_.fetch_add(1);
        creates_.fetch_add(1);
        auto const t0 = std::chrono::steady_clock::now();
        auto [valid, status] = session->doCreate(cache, params);
        noteSearchMs(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count()));
        if (valid)
        {
            std::lock_guard lock(subMutex_);
            forgetConnSessions(connId, cache);
            subs_[id] = Sub{session, std::move(push), connId};
            connToSession_.emplace(connId, id);
        }
        else
        {
            errors_.fetch_add(1);
        }
        return status;
    }
    if (sub == "close")
    {
        std::shared_ptr<xrpl::AssetCache> cache;
        {
            std::lock_guard lock(stateMutex_);
            cache = cache_;
        }
        std::lock_guard lock(subMutex_);
        json::Value last = xrpl::rpcError(xrpl::RpcNoPfRequest);
        auto range = connToSession_.equal_range(connId);
        for (auto it = range.first; it != range.second; ++it)
        {
            auto sit = subs_.find(it->second);
            if (sit != subs_.end())
                last = sit->second.session->doClose();
        }
        forgetConnSessions(connId, cache);
        return last;
    }
    if (sub == "status")
    {
        std::lock_guard lock(subMutex_);
        auto range = connToSession_.equal_range(connId);
        for (auto it = range.first; it != range.second; ++it)
        {
            auto sit = subs_.find(it->second);
            if (sit != subs_.end())
                return sit->second.session->doStatus();
        }
        return xrpl::rpcError(xrpl::RpcNoPfRequest);
    }
    return xrpl::rpcError(xrpl::RpcInvalidParams);
}

void
Engine::ripplePathFind(json::Value params, DoneFn done)
{
    try
    {
        pool_->submit([this, params = std::move(params), done = std::move(done)] {
            try
            {
                done(runRipplePathFind(params));
            }
            catch (std::exception const& ex)
            {
                errors_.fetch_add(1);
                auto err = xrpl::rpcError(xrpl::RpcInternal);
                err[xrpl::jss::error_message] = ex.what();
                done(std::move(err));
            }
        });
    }
    catch (std::exception const& ex)
    {
        errors_.fetch_add(1);
        auto err = xrpl::rpcError(xrpl::RpcInternal);
        err[xrpl::jss::error_message] = ex.what();
        done(std::move(err));
    }
}

void
Engine::pathFind(json::Value params, int connId, PushFn push, DoneFn done)
{
    auto const sub =
        params.isMember(xrpl::jss::subcommand) && params[xrpl::jss::subcommand].isString()
        ? params[xrpl::jss::subcommand].asString()
        : std::string{};
    if (sub == "close" || sub == "status")
    {
        done(runPathFind(params, connId, std::move(push)));
        return;
    }
    try
    {
        pool_->submit([this, params = std::move(params), connId, push = std::move(push), done = std::move(done)] {
            try
            {
                done(runPathFind(params, connId, push));
            }
            catch (std::exception const& ex)
            {
                errors_.fetch_add(1);
                auto err = xrpl::rpcError(xrpl::RpcInternal);
                err[xrpl::jss::error_message] = ex.what();
                done(std::move(err));
            }
        });
    }
    catch (std::exception const& ex)
    {
        errors_.fetch_add(1);
        auto err = xrpl::rpcError(xrpl::RpcInternal);
        err[xrpl::jss::error_message] = ex.what();
        done(std::move(err));
    }
}

void
Engine::setLedgerClosedHandler(std::function<void(json::Value)> handler)
{
    std::lock_guard lock(handlerMutex_);
    ledgerClosedHandler_ = std::move(handler);
}

json::Value
Engine::ledgerClosedJson() const
{
    std::lock_guard lock(stateMutex_);
    json::Value j{json::ValueType::Object};
    j[xrpl::jss::type] = "ledgerClosed";
    auto const seq = published_ ? published_->seq() : currentSeq_.load();
    if (seq == 0)
        return j;
    auto const first = firstSeq_.load() ? firstSeq_.load() : seq;
    j[xrpl::jss::ledger_index] = seq;
    if (published_)
        j[xrpl::jss::ledger_hash] = to_string(published_->header().hash);
    j[xrpl::jss::validated_ledgers] = std::to_string(first) + "-" + std::to_string(seq);
    if (published_)
    {
        j["reserve_base"] = static_cast<std::uint32_t>(published_->fees().reserve.drops());
        j["reserve_inc"] = static_cast<std::uint32_t>(published_->fees().increment.drops());
        j["fee_base"] = static_cast<std::uint32_t>(published_->fees().base.drops());
    }
    return j;
}

void
Engine::dropConnection(int connId)
{
    std::shared_ptr<xrpl::AssetCache> cache;
    {
        std::lock_guard lock(stateMutex_);
        cache = cache_;
    }
    std::lock_guard lock(subMutex_);
    forgetConnSessions(connId, cache);
}

}  // namespace edgy
