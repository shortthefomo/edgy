#include <edgy/graph.hpp>

#include <edgy/compat.hpp>
#include <edgy/order_books.hpp>
#include <edgy/ripple_calc.hpp>

#include <xrpld/rpc/detail/PathfinderUtils.h>
#include <xrpld/rpc/detail/Tuning.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/ledger/PaymentSandbox.h>
#include <xrpl/protocol/Book.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/tx/paths/RippleCalc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <string>
#include <unordered_set>
#include <vector>

namespace edgy {
namespace {

xrpl::STPathElement
bookElement(xrpl::Asset const& out)
{
    return bookPathElement(out);
}

xrpl::STPath
bookPath(std::vector<xrpl::Asset> const& hops)
{
    xrpl::STPath path;
    path.reserve(hops.size());
    for (auto const& hop : hops)
        pathPush(path, bookElement(hop));
    return path;
}

xrpl::STAmount
srcMaxAmount(
    xrpl::Asset const& srcAsset,
    xrpl::AccountID const& srcAccount,
    std::optional<xrpl::STAmount> const& sendMax)
{
    if (sendMax)
        return *sendMax;
    auto const sourceAccount = [&] {
        if (!xrpl::isXRP(srcAsset.getIssuer()))
            return srcAsset.getIssuer();
        if (xrpl::isXRP(srcAsset))
            return xrpl::xrpAccount();
        return srcAccount;
    }();
    return visitAsset(
        srcAsset,
        [&](xrpl::Issue const& issue) {
            return xrpl::STAmount(xrpl::Issue{issue.currency, sourceAccount}, 1u, 0, true);
        },
        [](xrpl::MPTIssue const& issue) { return xrpl::STAmount(issue, 1u, 0, true); });
}

void
addPath(xrpl::STPathSet& set, xrpl::STPath path)
{
    // Payment preclaim rejects path.size() > 8 and paths.size() > 6.
    if (path.empty() || path.size() > SearchBudget::kMaxPathLength)
        return;
    if (set.size() >= 256)
        return;
    pathSetPush(set, std::move(path));
}

// Currency sequence only — same hops / different issuers look identical
// in the UI and should not consume six path slots.
std::string
hopCurrencyKey(xrpl::STPath const& path)
{
    std::string key;
    key.reserve(path.size() * 16);
    for (auto const& el : path)
    {
        key += pathElementKey(el);
        key.push_back('/');
    }
    return key;
}

bool
seenOnPath(std::vector<xrpl::Asset> const& hops, xrpl::Asset const& asset)
{
    for (auto const& hop : hops)
    {
        if (hop == asset)
            return true;
    }
    return false;
}

std::vector<xrpl::Asset>
orderedNeighbors(
    LocalOrderBooks& books,
    xrpl::Asset const& from,
    xrpl::Asset const& dst,
    std::optional<xrpl::uint256> const& domain)
{
    auto out = books.neighbors(from, domain);
    std::ranges::stable_partition(out, [&](xrpl::Asset const& a) {
        return a == dst || xrpl::isXRP(a);
    });
    if (out.size() > 2)
    {
        auto first = out.begin();
        if (first != out.end() && (*first == dst || xrpl::isXRP(*first)))
            ++first;
        if (first != out.end() && (*first == dst || xrpl::isXRP(*first)))
            ++first;
        std::ranges::sort(first, out.end(), [&](xrpl::Asset const& a, xrpl::Asset const& b) {
            return books.getBookSize(a, domain) > books.getBookSize(b, domain);
        });
    }
    return out;
}

}  // namespace

SearchBudget
SearchBudget::forDepth(int depth)
{
    SearchBudget b;
    b.depth = std::clamp(depth, 0, kMaxDepth);
    switch (b.depth)
    {
        case 0:
            b.maxHops = 2;
            b.twoHop = 64;
            b.expand = 8;
            b.branch = 6;
            b.rank = 8;
            b.autoSources = 6;
            break;
        case 1:
            b.maxHops = 3;
            b.twoHop = 80;
            b.expand = 16;
            b.branch = 8;
            b.rank = 10;
            b.autoSources = 10;
            break;
        case 2:
            // Scan every 2-hop pair; RippleCalc only the best tips.
            b.maxHops = 4;
            b.twoHop = 128;
            b.expand = 16;
            b.branch = 8;
            b.rank = 10;
            b.autoSources = 14;
            break;
        case 3:
            b.maxHops = 6;
            b.twoHop = 128;
            b.expand = 32;
            b.branch = 8;
            b.rank = 12;
            b.autoSources = 18;
            break;
        default:
            b.maxHops = kMaxPathLength;
            b.twoHop = 128;
            b.expand = 48;
            b.branch = 10;
            b.rank = 12;
            b.autoSources = 18;
            break;
    }
    b.maxHops = std::clamp(b.maxHops, 1, kMaxPathLength);
    return b;
}

int
SearchBudget::depthFor(bool oneShot, int searchesDone, std::chrono::milliseconds age)
{
    // Legacy 3-arg: one-shot mid (2), live sockets climb 0 → 4.
    return depthFor(oneShot, searchesDone, age, oneShot ? 2 : 0, oneShot ? 2 : kMaxDepth);
}

int
SearchBudget::depthFor(
    bool oneShot,
    int searchesDone,
    std::chrono::milliseconds age,
    int searchFast,
    int search)
{
    int const hi = std::clamp(search, 0, kMaxDepth);
    int const lo = std::clamp(searchFast, 0, hi);
    if (oneShot)
        return hi;
    if (lo == hi)
        return hi;

    int fromUpdates = std::clamp(lo + searchesDone, lo, hi);
    int fromAge = lo;
    int const span = hi - lo;
    if (age >= std::chrono::seconds{50})
        fromAge = hi;
    else if (age >= std::chrono::seconds{25})
        fromAge = lo + std::max(1, (span * 3) / 4);
    else if (age >= std::chrono::seconds{12})
        fromAge = lo + std::max(1, (span * 2) / 4);
    else if (age >= std::chrono::seconds{4})
        fromAge = lo + std::max(1, span / 4);
    return std::clamp(std::max(fromUpdates, fromAge), lo, hi);
}

FastPathResult
FastPathFinder::search(
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
    std::function<bool()> const& continueCallback)
{
    FastPathResult result;
    result.depth = budget.depth;
    auto const t0 = std::chrono::steady_clock::now();
    auto j = services.getJournal("FastPath");

    if (!ledger)
        return result;

    auto const dstAsset = dstAmount.asset();
    xrpl::Asset const xrp{xrpl::xrpIssue()};
    int const maxHops = std::clamp(budget.maxHops, 1, SearchBudget::kMaxPathLength);
    auto const srcIsXrp = xrpl::isXRP(srcAsset);
    auto const dstIsXrp = xrpl::isXRP(dstAsset);

    xrpl::STPathSet candidates;

    auto edgeQ = [&](xrpl::Asset const& in, xrpl::Asset const& out) -> std::uint64_t {
        auto q = books.tipQuality(in, out, domain);
        return q == LocalOrderBooks::kNoQuality ? (LocalOrderBooks::kNoQuality / 2) : q;
    };
    auto pathQ = [&](std::vector<xrpl::Asset> const& hops) -> std::uint64_t {
        if (hops.empty())
            return LocalOrderBooks::kNoQuality;
        std::uint64_t q = edgeQ(srcAsset, hops.front());
        for (std::size_t i = 1; i < hops.size(); ++i)
        {
            auto const nq = edgeQ(hops[i - 1], hops[i]);
            q = nq > LocalOrderBooks::kNoQuality - q ? LocalOrderBooks::kNoQuality : q + nq;
        }
        return q;
    };

    struct Scored
    {
        std::uint64_t score{LocalOrderBooks::kNoQuality};
        std::vector<xrpl::Asset> hops;
    };
    std::vector<Scored> pairs;
    pairs.reserve(static_cast<std::size_t>(budget.twoHop) + 2);

    if (books.hasBook(srcAsset, dstAsset, domain))
        pairs.push_back({.score = pathQ({dstAsset}), .hops = {dstAsset}});

    if (!srcIsXrp && !dstIsXrp && books.hasBook(srcAsset, xrp, domain) &&
        books.hasBook(xrp, dstAsset, domain))
    {
        pairs.push_back({.score = pathQ({xrp, dstAsset}), .hops = {xrp, dstAsset}});
    }

    {
        int n = 0;
        for (auto const& mid : books.intermediates(srcAsset, dstAsset, domain))
        {
            if (continueCallback && !continueCallback())
                break;
            if (mid == srcAsset || mid == dstAsset || xrpl::equalTokens(mid, srcAsset) ||
                xrpl::equalTokens(mid, dstAsset))
                continue;
            if (!srcIsXrp && !dstIsXrp && xrpl::isXRP(mid))
                continue;
            pairs.push_back({.score = pathQ({mid, dstAsset}), .hops = {mid, dstAsset}});
            if (++n >= budget.twoHop)
                break;
        }
    }

    std::ranges::sort(pairs, [](Scored const& a, Scored const& b) {
        if (a.score != b.score)
            return a.score < b.score;
        return a.hops.size() < b.hops.size();
    });
    for (auto const& p : pairs)
        addPath(candidates, bookPath(p.hops));

    if (maxHops >= 3 && !dstIsXrp && books.hasBook(xrp, dstAsset, domain))
    {
        int n = 0;
        for (auto const& mid : books.intermediates(srcAsset, xrp, domain))
        {
            if (continueCallback && !continueCallback())
                break;
            if (xrpl::isXRP(mid) || xrpl::equalTokens(mid, srcAsset) ||
                xrpl::equalTokens(mid, dstAsset))
                continue;
            addPath(candidates, bookPath({mid, xrp, dstAsset}));
            if (++n >= budget.twoHop / 2)
                break;
        }
    }

    if (maxHops >= 3 && !srcIsXrp && books.hasBook(srcAsset, xrp, domain))
    {
        int n = 0;
        for (auto const& mid : books.intermediates(xrp, dstAsset, domain))
        {
            if (continueCallback && !continueCallback())
                break;
            if (xrpl::isXRP(mid) || xrpl::equalTokens(mid, srcAsset) ||
                xrpl::equalTokens(mid, dstAsset))
                continue;
            addPath(candidates, bookPath({xrp, mid, dstAsset}));
            if (++n >= budget.twoHop / 2)
                break;
        }
    }

    if (maxHops >= 4)
    {
        struct Frame
        {
            xrpl::Asset at;
            std::vector<xrpl::Asset> hops;
        };
        std::deque<Frame> q;
        q.push_back(Frame{srcAsset, {}});
        int expanded = 0;
        while (!q.empty() && expanded < budget.expand)
        {
            if (continueCallback && !continueCallback())
                break;
            auto const frame = std::move(q.front());
            q.pop_front();
            if (static_cast<int>(frame.hops.size()) >= maxHops)
                continue;
            auto const neigh = orderedNeighbors(books, frame.at, dstAsset, domain);
            int branched = 0;
            for (auto const& next : neigh)
            {
                if (next == srcAsset || xrpl::equalTokens(next, srcAsset) ||
                    seenOnPath(frame.hops, next))
                    continue;
                auto hops = frame.hops;
                hops.push_back(next);
                if (static_cast<int>(hops.size()) > maxHops)
                    continue;
                if (next == dstAsset)
                    addPath(candidates, bookPath(hops));
                else if (static_cast<int>(hops.size()) < maxHops)
                    q.push_back(Frame{next, std::move(hops)});
                if (++branched >= budget.branch)
                    break;
            }
            ++expanded;
        }
    }

    for (auto const& path : extra)
    {
        if (path.size() <= SearchBudget::kMaxPathLength)
            addPath(candidates, path);
    }

    result.candidates = static_cast<int>(candidates.size());
    result.search = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    if (candidates.empty())
        return result;

    auto const tRank = std::chrono::steady_clock::now();
    auto const saMax = srcMaxAmount(srcAsset, src, sendMax);
    auto const saMinDst = convertAll
        ? xrpl::largestAmount(dstAmount)
        : [&] {
              auto const slots = std::max(1, xrpl::rpc::tuning::kPathFindMaxPaths + 2);
              return divide(dstAmount, xrpl::STAmount(slots), dstAmount.asset());
          }();

    struct Rank
    {
        std::uint64_t quality{};
        xrpl::STAmount liquidity;
        std::size_t length{};
        int index{};
    };
    std::vector<Rank> ranks;
    ranks.reserve(std::min(candidates.size(), static_cast<std::size_t>(budget.rank)));

    int const limit = std::min(static_cast<int>(candidates.size()), budget.rank);
    std::unordered_set<std::string> rankedKeys;
    rankedKeys.reserve(static_cast<std::size_t>(limit));
    int ranked = 0;
    for (int i = 0; i < static_cast<int>(candidates.size()) && ranked < limit; ++i)
    {
        if (continueCallback && !continueCallback())
            break;
        auto const& path = candidates[static_cast<std::size_t>(i)];
        if (path.size() > SearchBudget::kMaxPathLength)
            continue;
        auto const sig = hopCurrencyKey(path);
        if (!rankedKeys.insert(sig).second)
            continue;
        ++ranked;
        xrpl::STPathSet one;
        pathSetPushAlways(one, path);

        xrpl::path::RippleCalc::Input rcInput;
        rcInput.defaultPathsAllowed = false;
        rcInput.partialPaymentAllowed = true;

        try
        {
            xrpl::PaymentSandbox sandbox(&*ledger, xrpl::TapNone);
            auto rc = rippleCalculate(
                sandbox, saMax, saMinDst, dst, src, one, domain, services, &rcInput);
            if (!xrpl::isTesSuccess(rc.result()))
                continue;

            ranks.push_back(Rank{
                .quality = xrpl::getRate(rc.actualAmountOut, rc.actualAmountIn),
                .liquidity = rc.actualAmountOut,
                .length = path.size(),
                .index = i});
        }
        catch (std::exception const& ex)
        {
            JLOG(j.debug()) << "fast path rank exception: " << ex.what();
        }
    }

    std::ranges::sort(ranks, [&](Rank const& a, Rank const& b) {
        if (!convertAll && a.quality != b.quality)
            return a.quality < b.quality;
        if (a.liquidity != b.liquidity)
            return a.liquidity > b.liquidity;
        if (a.length != b.length)
            return a.length < b.length;
        return a.index < b.index;
    });

    int const take = std::min(
        {static_cast<int>(ranks.size()),
         xrpl::rpc::tuning::kPathFindMaxPaths,
         SearchBudget::kMaxPathCount});
    std::unordered_set<std::string> chosen;
    auto pick = [&](bool shortOnly) {
        for (auto const& r : ranks)
        {
            if (static_cast<int>(result.paths.size()) >= take)
                return;
            auto const& path = candidates[static_cast<std::size_t>(r.index)];
            if (path.size() > SearchBudget::kMaxPathLength)
                continue;
            if (shortOnly && path.size() > 2)
                continue;
            if (!shortOnly && path.size() <= 2)
                continue;
            if (!chosen.insert(hopCurrencyKey(path)).second)
                continue;
            if (static_cast<int>(result.paths.size()) < SearchBudget::kMaxPathCount)
                pathSetPushAlways(result.paths, path);
            pathSetPushAlways(result.discovered, path);
        }
    };
    // Book pairs (1–2 hops) first so 3-hop hub chains cannot hide
    // the same 2-hop set xrpld Pathfinder returns.
    pick(true);
    pick(false);
    for (auto const& r : ranks)
    {
        if (static_cast<int>(result.discovered.size()) >= budget.rank)
            break;
        auto const& path = candidates[static_cast<std::size_t>(r.index)];
        if (path.size() > SearchBudget::kMaxPathLength)
            continue;
        if (!chosen.insert(hopCurrencyKey(path)).second)
            continue;
        pathSetPushAlways(result.discovered, path);
    }

    result.ranked = static_cast<int>(ranks.size());
    result.rank = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tRank);
    return result;
}

}  // namespace edgy
