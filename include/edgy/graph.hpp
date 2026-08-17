#pragma once

#include <edgy/services.hpp>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Asset.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STPathSet.h>
#include <xrpl/protocol/UintTypes.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace edgy {

class LocalOrderBooks;

/**
 * How wide a book-graph walk is allowed to go.
 *
 * WebSocket path_find starts at depth 0 (first reply stays fast) and
 * climbs as the subscription ages, up to the XRPL payment limits:
 * 8 steps per path (Payment::kMaxPathLength) and 6 paths per set
 * (Payment::kMaxPathSize). One-shot ripple_path_find uses a fixed
 * mid depth because it only gets one response.
 *
 * twoHop is how many scored 2-hop pairs to keep after ranking every
 * intersection — the walk itself is not truncated first.
 */
struct SearchBudget
{
    int depth{0};
    int maxHops{2};
    int twoHop{64};
    int expand{16};
    int branch{6};
    int rank{64};
    int autoSources{6};

    // Same as xrpl::Payment::kMaxPathLength / kMaxPathSize (private there).
    static constexpr int kMaxPathLength = 8;
    static constexpr int kMaxPathCount = 6;
    static constexpr int kMaxDepth = 4;

    [[nodiscard]] static SearchBudget
    forDepth(int depth);

    [[nodiscard]] static int
    depthFor(
        bool oneShot,
        int searchesDone,
        std::chrono::milliseconds age);

    // Climb from searchFast to search. One-shot uses `search`.
    [[nodiscard]] static int
    depthFor(
        bool oneShot,
        int searchesDone,
        std::chrono::milliseconds age,
        int searchFast,
        int search);
};

struct FastPathResult
{
    xrpl::STPathSet paths;
    xrpl::STPathSet discovered;
    int candidates{0};
    int ranked{0};
    int depth{0};
    bool isolateRank{false};
    std::chrono::milliseconds search{0};
    std::chrono::milliseconds rank{0};
};

/**
 * Book/AMM graph search. Scores every 1- and 2-hop pair (and a
 * bidirectional 3/4-hop meet) from composed tip/AMM quality and tip
 * size. 1–2 hop pairs stay in front of longer hops so speculative
 * 4-hop tips cannot hide the books xrpld returns. RippleCalc filters
 * the shortlist; only tesSUCCESS paths are returned.
 */
class FastPathFinder
{
public:
    static FastPathResult
    search(
        LocalOrderBooks& books,
        PathServices& services,
        std::shared_ptr<xrpl::ReadView const> const& ledger,
        xrpl::AccountID const& src,
        xrpl::AccountID const& dst,
        xrpl::Asset const& srcAsset,
        xrpl::STAmount const& dstAmount,
        std::optional<xrpl::STAmount> const& sendMax,
        std::optional<xrpl::uint256> const& domain,
        xrpl::STPathSet const& extra,
        bool convertAll,
        SearchBudget const& budget,
        std::function<bool()> const& continueCallback);
};

}  // namespace edgy
