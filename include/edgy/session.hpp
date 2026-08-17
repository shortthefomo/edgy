#pragma once

#include <edgy/compat.hpp>
#include <edgy/services.hpp>
#include <xrpld/rpc/detail/AssetCache.h>

#include <xrpl/json/json_value.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Asset.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STPathSet.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace edgy {

struct Config;

/**
 * True when a subscription should run FastPathFinder again instead of
 * only RippleCalc on the last hop list. lastFull == 0 means no search
 * has finished yet (create / deepen handles that). Due ledger is
 *   lastFull + interval + (sessionId % interval)
 * so sockets do not all rediscover on the same close.
 */
[[nodiscard]] inline bool
rediscoveryDue(
    xrpl::LedgerIndex lastFull,
    xrpl::LedgerIndex ledgerSeq,
    int sessionId,
    std::uint32_t interval) noexcept
{
    if (lastFull == 0)
        return false;
    auto const iv = interval == 0 ? 1u : interval;
    auto const id = sessionId < 0 ? 0 : static_cast<unsigned>(sessionId);
    return ledgerSeq >= lastFull + iv + static_cast<xrpl::LedgerIndex>(id % iv);
}

/**
 * One path_find / ripple_path_find request.
 *
 * JSON parse, ranking, and result fields are copied from xrpld PathRequest
 * so alternatives / source_amount / paths_computed / full_reply / warnings
 * match the node.
 */
class PathSession
{
public:
    PathSession(
        PathServices& services,
        Config const& cfg,
        int id,
        bool oneShot,
        beast::Journal journal);

    /**
     * Parse create params. Returns {valid, status}.
     * For WebSocket create, runs a shallow in-memory book-graph search.
     * Later subscription updates deepen the walk the longer the socket stays open.
     */
    std::pair<bool, json::Value>
    doCreate(std::shared_ptr<xrpl::AssetCache> const& cache, json::Value const& params);

    json::Value
    doClose();

    json::Value
    doStatus();

    json::Value
    doUpdate(
        std::shared_ptr<xrpl::AssetCache> const& cache,
        bool fast,
        bool revalidateOnly = false,
        std::shared_ptr<xrpl::ReadView const> const& calcLedger = {});

    [[nodiscard]] int
    id() const
    {
        return id_;
    }

    [[nodiscard]] bool
    oneShot() const
    {
        return oneShot_;
    }

    [[nodiscard]] bool
    closing() const
    {
        return closing_.load(std::memory_order_acquire);
    }

    // True when wall-clock age has crossed the next hop-budget band.
    [[nodiscard]] bool
    shouldDeepen() const;

    // True when this socket should rediscover hops (not only reprice
    // context_). Staggered by id; false until the first full search.
    [[nodiscard]] bool
    shouldRediscover(xrpl::LedgerIndex ledgerSeq) const;

    [[nodiscard]] bool
    tryBeginUpdate()
    {
        bool expected = false;
        return updating_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    void
    endUpdate()
    {
        updating_.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool
    updating() const
    {
        return updating_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int
    lastDepth() const
    {
        return lastDepth_.load(std::memory_order_acquire);
    }

private:
    int
    parseJson(json::Value const& params);

    bool
    isValid(std::shared_ptr<xrpl::AssetCache> const& cache);

    bool
    findPaths(
        std::shared_ptr<xrpl::AssetCache> const& cache,
        json::Value& jvArray,
        bool fullSearch,
        bool allowEscalate,
        bool forceFast,
        bool& didFullSearch,
        std::shared_ptr<xrpl::ReadView const> const& calcLedger,
        std::function<bool()> const& continueCallback);

    bool
    revalidatePaths(
        std::shared_ptr<xrpl::AssetCache> const& cache,
        xrpl::Asset const& asset,
        xrpl::STPathSet const& paths,
        xrpl::STAmount const& dstAmount,
        json::Value& jvArray,
        std::shared_ptr<xrpl::ReadView const> const& calcLedger);

    PathServices& registry_;
    Config const& cfg_;
    beast::Journal journal_;
    int const id_;
    bool const oneShot_;

    std::recursive_mutex lock_;

    json::Value jvId_;
    json::Value jvStatus_{kJsonObject};

    std::optional<xrpl::AccountID> src_;
    std::optional<xrpl::AccountID> dst_;
    xrpl::STAmount dstAmount_;
    std::optional<xrpl::STAmount> sendMax_;
    std::set<xrpl::Asset> sourceAssets_;
    std::map<xrpl::Asset, xrpl::STPathSet> context_;
    std::optional<xrpl::uint256> domain_;
    bool convertAll_{false};
    bool sourceCurrenciesTruncated_{false};

    bool lastSuccess_{false};
    xrpl::LedgerIndex lastFullSearchIndex_{0};
    std::uint64_t lastLineEpoch_{0};
    std::atomic<bool> closing_{false};
    std::atomic<bool> updating_{false};

    std::chrono::steady_clock::time_point createdAt_{std::chrono::steady_clock::now()};
    int searchesDone_{0};
    std::atomic<int> lastDepth_{0};
};

}  // namespace edgy
