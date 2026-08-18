#pragma once

#include <edgy/config.hpp>
#include <edgy/memory_ledger.hpp>
#include <edgy/node_client.hpp>
#include <edgy/protocol.hpp>
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
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace edgy {

class ThreadPool;

// Apply-thread cycle after wait_for(update-ms). Empty queue must still
// tick so path_find subscriptions reprice every 100ms, not only on close.
struct ApplyCyclePlan
{
    bool exit{false};
    bool hold{false};
    bool takeBatch{false};
    bool tick{false};
};

[[nodiscard]] inline ApplyCyclePlan
planApplyCycle(bool stop, bool ready, bool queueEmpty) noexcept
{
    if (stop && queueEmpty)
        return {.exit = true};
    if (!ready)
        return {.hold = true};
    return {
        .exit = false,
        .hold = false,
        .takeBatch = !queueEmpty,
        .tick = !stop,
    };
}

// xrpld publishes ledgerClosed for N, then the validated txs for N.
// Skip older ledgers (and the snapshot ledger). Hold a newer ledger's
// txs until that ledger closes — applying them early dirties Indexes
// and Balance when we digest N against the node.
[[nodiscard]] inline bool
shouldApplyStreamTx(
    std::uint32_t txSeq,
    std::uint32_t currentSeq,
    std::uint32_t snapshotSeq = 0) noexcept
{
    if (txSeq == 0)
        return true;
    if (snapshotSeq != 0 && txSeq <= snapshotSeq)
        return false;
    return txSeq == currentSeq;
}

[[nodiscard]] inline bool
shouldDeferStreamTx(
    std::uint32_t txSeq,
    std::uint32_t currentSeq,
    std::uint32_t snapshotSeq = 0) noexcept
{
    if (txSeq == 0)
        return false;
    if (snapshotSeq != 0 && txSeq <= snapshotSeq)
        return false;
    return txSeq > currentSeq;
}

// Local vs node object. Blob hashes differ after apply because we
// re-serialize metadata; that is Reencoded, not drift. Drift is a missing
// object or a node field we do not have / have a different value for.
enum class ObjectDigestCmp : std::uint8_t
{
    Match = 0,
    MissingLocal,
    MissingNode,
    Mismatch,
    Reencoded,
};

[[nodiscard]] inline ObjectDigestCmp
compareObjectDigest(
    std::optional<xrpl::uint256> const& local,
    std::optional<xrpl::uint256> const& node) noexcept
{
    if (!local && !node)
        return ObjectDigestCmp::Match;
    if (!local)
        return ObjectDigestCmp::MissingLocal;
    if (!node)
        return ObjectDigestCmp::MissingNode;
    return *local == *node ? ObjectDigestCmp::Match : ObjectDigestCmp::Mismatch;
}

[[nodiscard]] inline ObjectDigestCmp
compareAppliedObject(
    std::optional<xrpl::uint256> const& localDigest,
    std::optional<xrpl::uint256> const& nodeDigest,
    xrpl::STObject const* local,
    xrpl::STObject const* node)
{
    if (!local && !node)
        return ObjectDigestCmp::Match;
    if (!local)
        return ObjectDigestCmp::MissingLocal;
    if (!node)
        return ObjectDigestCmp::MissingNode;
    if (localDigest && nodeDigest && *localDigest == *nodeDigest)
        return ObjectDigestCmp::Match;
    if (sleCoversNode(*local, *node))
        return ObjectDigestCmp::Reencoded;
    return ObjectDigestCmp::Mismatch;
}

[[nodiscard]] inline bool
objectDigestIsDrift(ObjectDigestCmp c) noexcept
{
    return c == ObjectDigestCmp::Mismatch || c == ObjectDigestCmp::MissingLocal ||
        c == ObjectDigestCmp::MissingNode;
}

inline constexpr int kDigestDriftResync = 2;

// nodeTxs is the previous close's txn_count. applyTxs is txs we processed;
// noMeta is txs that arrived with no meta at all. Per-node parseFail must
// not be added — that made txs=N/N warn txs!=N after every close.
[[nodiscard]] inline bool
applyTxsMatchNode(
    std::uint64_t applyTxs,
    std::uint64_t noMeta,
    std::uint32_t nodeTxs) noexcept
{
    return nodeTxs == 0 || applyTxs + noMeta == nodeTxs;
}

// xrpld sends ledgerClosed N, then the validated txs for N. Checking at
// close compares local N-1 to node N (Balance/Indexes look wrong). Digest
// after the close's txn_count has been applied, or immediately when the
// close has no txs.
[[nodiscard]] inline bool
shouldDigestOnClose(std::uint32_t txnCount) noexcept
{
    return txnCount == 0;
}

[[nodiscard]] inline bool
shouldDigestAfterApply(
    std::uint64_t applyTxs,
    std::uint64_t noMeta,
    std::uint32_t expectedTxs,
    std::uint32_t currentSeq,
    std::uint32_t lastDigestSeq) noexcept
{
    if (expectedTxs == 0 || currentSeq == 0 || currentSeq == lastDigestSeq)
        return false;
    return applyTxsMatchNode(applyTxs, noMeta, expectedTxs);
}

// Apply stream JSON meta (AffectedNodes). Used by the live apply path and tests.
struct StreamMetaStats
{
    std::uint32_t created{0};
    std::uint32_t modified{0};
    std::uint32_t deleted{0};
    std::uint32_t incomplete{0};
    std::uint32_t none{0};
    std::uint32_t parseFail{0};
    std::uint32_t skippedUnknown{0};

    [[nodiscard]] std::uint32_t
    applied() const
    {
        return created + modified + deleted + incomplete;
    }
};

StreamMetaStats
applyJsonAffectedNodes(
    LedgerBuilder& builder,
    LocalOrderBooks& books,
    json::Value const& meta);

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
    scheduleDigestCheck();

    void
    runDigestCheck(std::shared_ptr<MemoryLedger const> const& view);

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
    std::atomic<std::uint32_t> snapshotSeq_{0};
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
    std::uint64_t applyLedgerTxs_{0};
    std::uint64_t applyNoMeta_{0};
    std::uint64_t applyParseFail_{0};
    std::uint64_t applySkipped_{0};
    std::uint32_t prevCloseTxs_{0};
    std::uint64_t applyCreated_{0};
    std::uint64_t applyDeleted_{0};
    std::uint64_t applyModified_{0};
    std::uint64_t applyIncomplete_{0};
    std::vector<json::Value> pendingFutureTxs_;

    std::atomic<bool> digestCheckBusy_{false};
    std::atomic<std::uint32_t> digestChecked_{0};
    std::atomic<std::uint32_t> digestMatched_{0};
    std::atomic<std::uint32_t> digestMismatch_{0};
    std::atomic<std::uint32_t> digestMissingLocal_{0};
    std::atomic<std::uint32_t> digestMissingNode_{0};
    std::atomic<std::uint32_t> digestReencoded_{0};
    std::atomic<std::uint32_t> digestLastSeq_{0};
    std::atomic<std::uint32_t> digestDoneSeq_{0};
    std::atomic<bool> digestDrift_{false};

    void
    maybeDigestAfterApply();

    void
    resetApplyStats();

    void
    logSync(char const* what, xrpl::LedgerHeader const& header, json::Value const& nodeMsg);
};

}  // namespace edgy
