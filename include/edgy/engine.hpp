#pragma once

#include <edgy/config.hpp>
#include <edgy/memory_ledger.hpp>
#include <edgy/node_client.hpp>
#include <edgy/services.hpp>
#include <edgy/session.hpp>

#include <xrpld/rpc/detail/AssetCache.h>

#include <xrpl/json/json_value.h>
#include <xrpl/ledger/ReadView.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace edgy {

class ThreadPool;

class Engine
{
public:
    using PushFn = std::function<void(json::Value)>;
    using DoneFn = std::function<void(json::Value)>;

    Engine(boost::asio::io_context& io, Config cfg, std::shared_ptr<NodeClient> node);
    ~Engine();

    Engine(Engine const&) = delete;
    Engine&
    operator=(Engine const&) = delete;

    void
    start();

    void
    stop();

    [[nodiscard]] bool
    ready() const;

    [[nodiscard]] json::Value
    statusJson() const;

    // Flat counters for path_counts / get_counts (graphable gauges + totals).
    [[nodiscard]] json::Value
    pathCountsJson() const;

    void
    ripplePathFind(json::Value params, DoneFn done);

    void
    pathFind(json::Value params, int connId, PushFn push, DoneFn done);

    void
    dropConnection(int connId);

    void
    setLedgerClosedHandler(std::function<void(json::Value)> handler);

    [[nodiscard]] json::Value
    ledgerClosedJson() const;

    [[nodiscard]] PathServices&
    services()
    {
        return services_;
    }

    [[nodiscard]] std::shared_ptr<xrpl::ReadView const>
    ledger() const;

private:
    void
    syncLoop();

    void
    applyLoop();

    void
    loadSnapshot();

    void
    enqueueApply(bool ledgerClosed, json::Value msg);

    void
    applyLedgerClosed(json::Value const& msg);

    void
    applyTransaction(json::Value const& msg);

    void
    publishBuilder(bool rebuildBooks);

    void
    notifySubscriptions(bool revalidateOnly);

    void
    midCloseTick();

    void
    waitApplyIdle();

    void
    forgetConnSessions(int connId, std::shared_ptr<xrpl::AssetCache> const& cache);

    json::Value
    runRipplePathFind(json::Value const& params);

    json::Value
    runPathFind(json::Value const& params, int connId, PushFn push);

    void
    noteSearchMs(std::uint64_t ms);

    Config cfg_;
    std::chrono::steady_clock::time_point startedAt_{std::chrono::steady_clock::now()};
    std::shared_ptr<NodeClient> node_;
    PathServices services_;
    std::unique_ptr<ThreadPool> pool_;

    mutable std::mutex stateMutex_;
    LedgerBuilder builder_;
    std::shared_ptr<MemoryLedger const> published_;
    std::shared_ptr<xrpl::AssetCache> cache_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> searches_{0};
    std::atomic<std::uint64_t> creates_{0};
    std::atomic<std::uint64_t> oneShots_{0};
    std::atomic<std::uint64_t> updates_{0};
    std::atomic<std::uint64_t> revalidates_{0};
    std::atomic<std::uint64_t> deepens_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> searchMsSum_{0};
    std::atomic<std::uint64_t> searchMsLast_{0};
    std::atomic<std::uint64_t> searchMsMax_{0};
    std::atomic<std::uint64_t> objects_{0};
    std::atomic<std::uint32_t> firstSeq_{0};
    std::atomic<std::uint32_t> currentSeq_{0};
    std::function<void(json::Value)> ledgerClosedHandler_;
    std::mutex handlerMutex_;

    mutable std::mutex subMutex_;
    struct Sub
    {
        std::shared_ptr<PathSession> session;
        PushFn push;
        int connId{0};
    };
    std::unordered_map<int, Sub> subs_;
    std::unordered_multimap<int, int> connToSession_;
    std::atomic<int> nextId_{1};

    std::thread syncThread_;
    std::thread applyThread_;

    struct ApplyItem
    {
        bool ledgerClosed{false};
        json::Value msg;
    };
    mutable std::mutex applyMutex_;
    std::condition_variable applyCv_;
    std::queue<ApplyItem> applyQueue_;
    std::atomic<bool> dirty_{false};
    std::atomic<bool> needResync_{false};
    std::atomic<bool> applyIdle_{true};
    std::chrono::steady_clock::time_point lastTick_{};

    // Apply-thread only. Reset after each closed ledger / snapshot.
    std::uint64_t applyTxs_{0};
    std::uint64_t applyNoMeta_{0};
    std::uint64_t applyCreated_{0};
    std::uint64_t applyDeleted_{0};
    std::uint64_t applyModified_{0};
    std::uint64_t applyIncomplete_{0};

    void
    resetApplyStats();

    void
    logSync(char const* what, xrpl::LedgerHeader const& header, json::Value const& nodeMsg);
};

}  // namespace edgy
