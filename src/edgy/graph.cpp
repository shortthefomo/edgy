#include <edgy/graph.hpp>

#include <edgy/book_util.hpp>
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
#include <xrpl/protocol/Quality.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/tx/paths/RippleCalc.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace edgy {
namespace {

constexpr std::uint16_t kAmmFeeDivisor = 100'000;

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

// Prepend the source IOU issuer (Pathfinder source_: rMx / RLUSD / rMx).
// Distinct from issuerBridgePath, which inserts the *mid* issuer after a book.
xrpl::STPath
prefixSourceIssuer(xrpl::STPath path, xrpl::Asset const& srcAsset)
{
    if (path.size() + 1 > SearchBudget::kMaxPathLength || !canIssuerHop(srcAsset))
        return path;
    xrpl::STPath out;
    out.reserve(path.size() + 1);
    pathPush(out, sourceIssuerPathElement(srcAsset));
    for (auto const& el : path)
        pathPush(out, el);
    return out;
}

xrpl::STPath
issuerBridgePath(std::vector<xrpl::Asset> const& hops)
{
    xrpl::STPath path;
    path.reserve(hops.size() * 2);
    for (std::size_t i = 0; i < hops.size(); ++i)
    {
        pathPush(path, bookElement(hops[i]));
        if (i + 1 >= hops.size() || !canIssuerHop(hops[i]))
            continue;
        auto remaining = hops.size() - i - 1;
        if (path.size() + 1 + remaining > SearchBudget::kMaxPathLength)
            continue;
        pathPush(path, accountPathElement(hops[i]));
    }
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

xrpl::Quality
worstQuality()
{
    return xrpl::Quality{LocalOrderBooks::kNoQuality / 2};
}

bool
isConvertAllAmount(xrpl::STAmount const& amt)
{
    return amt == xrpl::STAmount(amt.asset(), 1u, 0, true);
}

std::optional<xrpl::STAmount>
ammHolds(
    std::shared_ptr<xrpl::ReadView const> const& ledger,
    xrpl::AccountID const& acct,
    xrpl::Asset const& asset)
{
    if (!ledger)
        return std::nullopt;
    if (xrpl::isXRP(asset))
    {
        auto const root = ledger->read(xrpl::keylet::account(acct));
        if (!root || !root->isFieldPresent(xrpl::sfBalance))
            return std::nullopt;
        return root->getFieldAmount(xrpl::sfBalance);
    }
    return visitAsset(
        asset,
        [&](xrpl::Issue const& issue) -> std::optional<xrpl::STAmount> {
            auto const line = ledger->read(trustLineKeylet(acct, issue));
            if (!line || !line->isFieldPresent(xrpl::sfBalance))
                return std::nullopt;
            auto bal = line->getFieldAmount(xrpl::sfBalance);
            // RippleState balance is from the low account's perspective.
            if (acct > issue.account)
                bal.negate();
            if (bal.signum() < 0)
                bal = xrpl::STAmount{bal.asset()};
            return bal;
        },
        [](xrpl::MPTIssue const&) -> std::optional<xrpl::STAmount> { return std::nullopt; });
}

xrpl::STAmount
amountFromDouble(xrpl::Asset const& asset, double v)
{
    if (!(v > 0) || !std::isfinite(v))
        return xrpl::STAmount{asset};
    int const exp = static_cast<int>(std::floor(std::log10(v)));
    double const scaled = v / std::pow(10.0, exp);
    auto const mant = static_cast<std::uint64_t>(std::llround(scaled * 1'000'000'000'000'000.0));
    return xrpl::STAmount(asset, mant == 0 ? 1 : mant, exp - 15, false);
}

struct AmmQuote
{
    xrpl::Quality quality{worstQuality()};
    double outSize{0};
};

std::optional<AmmQuote>
ammQuote(
    std::shared_ptr<xrpl::ReadView const> const& ledger,
    xrpl::Asset const& in,
    xrpl::Asset const& out,
    double outNeeded)
{
    if (!ledger)
        return std::nullopt;
    auto const sle = ledger->read(xrpl::keylet::amm(in, out));
    if (!sle || !sle->isFieldPresent(xrpl::sfAccount))
        return std::nullopt;
    auto const acct = sle->getAccountID(xrpl::sfAccount);
    auto const inBal = ammHolds(ledger, acct, in);
    auto const outBal = ammHolds(ledger, acct, out);
    if (!inBal || !outBal || inBal->signum() <= 0 || outBal->signum() <= 0)
        return std::nullopt;

    double const x = amountAsDouble(*inBal);
    double const y = amountAsDouble(*outBal);
    if (!(x > 0) || !(y > 0))
        return std::nullopt;

    std::uint16_t fee = 0;
    if (sle->isFieldPresent(xrpl::sfTradingFee))
        fee = sle->getFieldU16(xrpl::sfTradingFee);
    double const keep = 1.0 - (static_cast<double>(fee) / static_cast<double>(kAmmFeeDivisor));
    if (keep <= 0)
        return std::nullopt;

    AmmQuote q;
    q.outSize = y * 0.99;
    double dy = (outNeeded > 0 && outNeeded < q.outSize) ? outNeeded : 0;
    try
    {
        if (dy > 0)
        {
            double const dx = (x * dy / (y - dy)) / keep;
            q.quality = xrpl::Quality{
                xrpl::getRate(amountFromDouble(out, dy), amountFromDouble(in, dx))};
        }
        else
        {
            double const dx = (x / y) / keep;
            q.quality = xrpl::Quality{
                xrpl::getRate(amountFromDouble(out, 1.0), amountFromDouble(in, dx))};
        }
    }
    catch (...)
    {
        return std::nullopt;
    }
    return q;
}

struct EdgeQuote
{
    xrpl::Quality quality{worstQuality()};
    double outSize{0};
};

EdgeQuote
quoteEdge(
    LocalOrderBooks& books,
    std::shared_ptr<xrpl::ReadView const> const& ledger,
    xrpl::Asset const& in,
    xrpl::Asset const& out,
    std::optional<xrpl::uint256> const& domain,
    double outNeeded)
{
    EdgeQuote q;
    auto const tip = books.tip(in, out, domain);
    if (tip.quality != LocalOrderBooks::kNoQuality)
        q.quality = xrpl::Quality{tip.quality};
    q.outSize = tip.outSize;
    if (tip.amm)
    {
        if (auto amm = ammQuote(ledger, in, out, outNeeded))
        {
            if (amm->quality > q.quality)
                q.quality = amm->quality;
            if (amm->outSize > q.outSize)
                q.outSize = amm->outSize;
        }
        else if (tip.quality == LocalOrderBooks::kNoQuality)
        {
            // Known AMM but no reserves yet — keep it above a missing book.
            q.quality = xrpl::Quality{LocalOrderBooks::kNoQuality / 4};
        }
    }
    return q;
}

struct PathScore
{
    xrpl::Quality quality{worstQuality()};
    double destWidth{0};
    std::vector<xrpl::Asset> hops;
};

std::optional<PathScore>
scoreHops(
    LocalOrderBooks& books,
    std::shared_ptr<xrpl::ReadView const> const& ledger,
    xrpl::Asset const& srcAsset,
    xrpl::Asset const& dstAsset,
    std::vector<xrpl::Asset> hops,
    std::optional<xrpl::uint256> const& domain,
    double destNeeded,
    std::optional<xrpl::Quality> const& bound)
{
    if (hops.empty() || hops.size() > static_cast<std::size_t>(SearchBudget::kMaxPathLength))
        return std::nullopt;

    // Work backwards so AMM quotes see an estimated out volume.
    std::vector<double> needed(hops.size(), 0);
    needed.back() = destNeeded;
    std::vector<EdgeQuote> quotes(hops.size());
    for (std::size_t i = hops.size(); i-- > 0;)
    {
        auto const in = (i == 0) ? srcAsset : hops[i - 1];
        quotes[i] = quoteEdge(books, ledger, in, hops[i], domain, needed[i]);
        if (i > 0)
        {
            auto const ratio = qualityRatio(quotes[i].quality);
            needed[i - 1] = (destNeeded > 0 && ratio > 0) ? destNeeded * ratio : 0;
        }
    }

    xrpl::Quality composed = quotes.front().quality;
    if (bound && composed < *bound)
        return std::nullopt;
    for (std::size_t i = 1; i < quotes.size(); ++i)
    {
        composed = composeQuality(composed, quotes[i].quality);
        if (bound && composed < *bound)
            return std::nullopt;
    }

    // Bottleneck width in dest units.
    double width = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < hops.size(); ++i)
    {
        double const out = quotes[i].outSize;
        if (!(out > 0))
            continue;
        double destEq = out;
        if (i + 1 < quotes.size())
        {
            xrpl::Quality tail = quotes[i + 1].quality;
            for (std::size_t j = i + 2; j < quotes.size(); ++j)
                tail = composeQuality(tail, quotes[j].quality);
            auto const r = qualityRatio(tail);
            destEq = r > 0 ? out / r : 0;
        }
        if (destEq > 0 && destEq < width)
            width = destEq;
    }
    if (!std::isfinite(width))
        width = 0;

    return PathScore{.quality = composed, .destWidth = width, .hops = std::move(hops)};
}

bool
betterScore(PathScore const& a, PathScore const& b, bool convertAll, double minUseful)
{
    if (convertAll)
    {
        if (a.destWidth != b.destWidth)
            return a.destWidth > b.destWidth;
        if (a.quality != b.quality)
            return a.quality > b.quality;
        if (a.hops.size() != b.hops.size())
            return a.hops.size() < b.hops.size();
        return false;
    }
    bool const aThin = minUseful > 0 && a.destWidth > 0 && a.destWidth < minUseful;
    bool const bThin = minUseful > 0 && b.destWidth > 0 && b.destWidth < minUseful;
    if (aThin != bThin)
        return !aThin;
    if (a.quality != b.quality)
        return a.quality > b.quality;
    if (a.destWidth != b.destWidth)
        return a.destWidth > b.destWidth;
    if (a.hops.size() != b.hops.size())
        return a.hops.size() < b.hops.size();
    return false;
}

std::vector<xrpl::Asset>
hopsFromPath(xrpl::STPath const& path)
{
    std::vector<xrpl::Asset> hops;
    hops.reserve(path.size());
    for (auto const& el : path)
    {
        if (pathElementIsAccount(el))
            continue;
        hops.push_back(pathElementAsset(el));
    }
    return hops;
}

}  // namespace

SearchBudget
SearchBudget::forDepth(int depth, int exploreWave)
{
    SearchBudget b;
    b.depth = std::clamp(depth, 0, kMaxDepth);
    b.exploreWave = std::max(0, exploreWave);
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
            // Score every 2-hop pair; keep this many for Flow.
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
    // Later WS waves open a bit more hop room and rotate cost into
    // expand, not a second full 2-hop scan of every mid.
    if (b.exploreWave > 0)
    {
        b.maxHops = std::max(b.maxHops, std::min(kMaxPathLength, 4 + (b.exploreWave % 3)));
        b.expand = std::min(64, b.expand + 8 * (b.exploreWave % 4));
        b.rank = std::min(16, b.rank + (b.exploreWave % 3));
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
    result.isolateRank = true;
    auto const t0 = std::chrono::steady_clock::now();
    auto j = services.getJournal("FastPath");

    if (!ledger)
        return result;

    auto const dstAsset = dstAmount.asset();
    xrpl::Asset const xrp{xrpl::xrpIssue()};
    int const maxHops = std::clamp(budget.maxHops, 1, SearchBudget::kMaxPathLength);
    auto const srcIsXrp = xrpl::isXRP(srcAsset);
    auto const dstIsXrp = xrpl::isXRP(dstAsset);
    bool const issuerIsSender = srcIsXrp || srcAsset.getIssuer() == src;

    std::optional<xrpl::Quality> bound;
    if (!convertAll && sendMax && !isConvertAllAmount(*sendMax) && sendMax->signum() > 0 &&
        dstAmount.signum() > 0)
    {
        bound = xrpl::Quality{xrpl::getRate(dstAmount, *sendMax)};
    }

    double const destNeeded = convertAll ? 0.0 : amountAsDouble(dstAmount);
    double const minUseful = convertAll
        ? 0.0
        : destNeeded / static_cast<double>(std::max(1, xrpl::rpc::tuning::kPathFindMaxPaths + 2));
    // Convert-all with a real send_max: rank by dest you get from that
    // input, not raw tip size (a fat EVR book looked "wider" than USD).
    double const sendIn = (convertAll && sendMax && sendMax->signum() > 0 &&
                           !isConvertAllAmount(*sendMax))
        ? amountAsDouble(*sendMax)
        : 0.0;

    // 1–2 hop books must stay ahead of 3–8 hop meets. A global quality
    // sort let optimistic 4-hop dust tips bury RLUSD→USD→XRP and then
    // the session Flow of those six junk paths returned alts=0.
    std::vector<PathScore> shortHops;
    std::vector<PathScore> longHops;
    shortHops.reserve(static_cast<std::size_t>(budget.twoHop) + 8);
    longHops.reserve(static_cast<std::size_t>(budget.twoHop) + 8);

    auto consider = [&](std::vector<xrpl::Asset> hops) {
        if (continueCallback && !continueCallback())
            return;
        auto const n = hops.size();
        if (auto s = scoreHops(
                books, ledger, srcAsset, dstAsset, std::move(hops), domain, destNeeded, bound))
        {
            if (sendIn > 0)
            {
                auto const r = qualityRatio(s->quality);
                double est = r > 0 ? sendIn / r : 0;
                if (s->destWidth > 0 && est > s->destWidth)
                    est = s->destWidth;
                if (est > 0)
                    s->destWidth = est;
            }
            if (n <= 2)
                shortHops.push_back(std::move(*s));
            else
                longHops.push_back(std::move(*s));
        }
    };

    // Always score the direct dest book / AMM and the XRP bridge.
    // Do not require hasBook — token vs Issue adjacency can miss an
    // edge that RippleCalc still pays (RLUSD→XRP, then XRP→XAH).
    if (srcAsset != dstAsset && !xrpl::equalTokens(srcAsset, dstAsset))
        consider({dstAsset});
    else if (books.hasBook(srcAsset, dstAsset, domain))
        consider({dstAsset});

    if (!srcIsXrp && !dstIsXrp && srcAsset != dstAsset)
        consider({xrp, dstAsset});

    // Score 2-hop meets. Wave 0 walks every mid; later waves take a
    // rotated window so rediscovery stays cheap and still finds new hubs.
    {
        std::vector<xrpl::Asset> mids;
        for (auto const& mid : books.intermediates(srcAsset, dstAsset, domain))
        {
            if (mid == srcAsset || mid == dstAsset || xrpl::equalTokens(mid, srcAsset) ||
                xrpl::equalTokens(mid, dstAsset))
                continue;
            if (!srcIsXrp && !dstIsXrp && xrpl::isXRP(mid))
                continue;
            mids.push_back(mid);
        }
        int const n = static_cast<int>(mids.size());
        int start = 0;
        int count = n;
        if (budget.exploreWave > 0 && n > budget.twoHop)
        {
            start = (budget.exploreWave * std::max(1, budget.twoHop / 2)) % n;
            count = budget.twoHop;
        }
        for (int i = 0; i < count; ++i)
        {
            if (continueCallback && !continueCallback())
                break;
            consider({mids[static_cast<std::size_t>((start + i) % n)], dstAsset});
        }
    }

    auto walkMids = [&](std::vector<xrpl::Asset> mids, auto const& use) {
        int const n = static_cast<int>(mids.size());
        int start = 0;
        int count = n;
        if (budget.exploreWave > 0 && n > budget.twoHop)
        {
            start = ((budget.exploreWave + 1) * std::max(1, budget.twoHop / 2)) % n;
            count = budget.twoHop;
        }
        for (int i = 0; i < count; ++i)
        {
            if (continueCallback && !continueCallback())
                break;
            use(mids[static_cast<std::size_t>((start + i) % n)]);
        }
    };

    if (maxHops >= 3 && !dstIsXrp && books.hasBook(xrp, dstAsset, domain))
    {
        std::vector<xrpl::Asset> mids;
        for (auto const& mid : books.intermediates(srcAsset, xrp, domain))
        {
            if (xrpl::isXRP(mid) || xrpl::equalTokens(mid, srcAsset) ||
                xrpl::equalTokens(mid, dstAsset))
                continue;
            mids.push_back(mid);
        }
        walkMids(std::move(mids), [&](xrpl::Asset const& mid) {
            consider({mid, xrp, dstAsset});
        });
    }

    if (maxHops >= 3 && !srcIsXrp && books.hasBook(srcAsset, xrp, domain))
    {
        std::vector<xrpl::Asset> mids;
        for (auto const& mid : books.intermediates(xrp, dstAsset, domain))
        {
            if (xrpl::isXRP(mid) || xrpl::equalTokens(mid, srcAsset) ||
                xrpl::equalTokens(mid, dstAsset))
                continue;
            mids.push_back(mid);
        }
        walkMids(std::move(mids), [&](xrpl::Asset const& mid) {
            consider({xrp, mid, dstAsset});
        });
    }

    // 4-hop meet-in-the-middle: src→A→B  ∩  X→M→dst with B==X.
    if (maxHops >= 4)
    {
        struct Leg
        {
            PathScore score;
            xrpl::Asset via{};
        };
        std::vector<Leg> prefixes;
        std::vector<Leg> suffixes;
        prefixes.reserve(static_cast<std::size_t>(budget.twoHop));
        suffixes.reserve(static_cast<std::size_t>(budget.twoHop));

        int const legCap = std::max(budget.twoHop * 4, 32);
        int const skipPref =
            budget.exploreWave > 0 ? (budget.exploreWave * 8) % std::max(1, legCap) : 0;
        int nPref = 0;
        int seenPref = 0;
        for (auto const& a : books.neighbors(srcAsset, domain))
        {
            if (continueCallback && !continueCallback())
                break;
            if (seenPref++ < skipPref)
                continue;
            if (nPref >= legCap)
                break;
            if (a == srcAsset || xrpl::equalTokens(a, srcAsset))
                continue;
            for (auto const& b : books.neighbors(a, domain))
            {
                if (nPref >= legCap)
                    break;
                if (b == srcAsset || b == a || xrpl::equalTokens(b, srcAsset))
                    continue;
                if (auto s = scoreHops(
                        books,
                        ledger,
                        srcAsset,
                        b,
                        {a, b},
                        domain,
                        0,
                        std::nullopt))
                {
                    prefixes.push_back(Leg{std::move(*s), b});
                    ++nPref;
                }
            }
        }
        int nSuf = 0;
        for (auto const& m : books.predecessors(dstAsset, domain))
        {
            if (continueCallback && !continueCallback())
                break;
            if (nSuf >= legCap)
                break;
            if (m == dstAsset || xrpl::equalTokens(m, dstAsset))
                continue;
            for (auto const& x : books.predecessors(m, domain))
            {
                if (nSuf >= legCap)
                    break;
                if (x == dstAsset || x == m || xrpl::equalTokens(x, dstAsset))
                    continue;
                if (auto s = scoreHops(
                        books,
                        ledger,
                        x,
                        dstAsset,
                        {m, dstAsset},
                        domain,
                        destNeeded,
                        bound))
                {
                    suffixes.push_back(Leg{std::move(*s), x});
                    ++nSuf;
                }
            }
        }

        auto takeLegs = [&](std::vector<Leg>& legs) {
            if (static_cast<int>(legs.size()) <= budget.twoHop)
                return;
            std::ranges::partial_sort(
                legs,
                legs.begin() + budget.twoHop,
                [&](Leg const& a, Leg const& b) {
                    return betterScore(a.score, b.score, convertAll, minUseful);
                });
            legs.resize(static_cast<std::size_t>(budget.twoHop));
        };
        takeLegs(prefixes);
        takeLegs(suffixes);

        std::unordered_multimap<std::string, Leg const*> byEnd;
        auto assetKey = [](xrpl::Asset const& a) { return to_string(a); };
        for (auto const& s : suffixes)
            byEnd.emplace(assetKey(s.via), &s);
        for (auto const& p : prefixes)
        {
            if (continueCallback && !continueCallback())
                break;
            auto range = byEnd.equal_range(assetKey(p.via));
            for (auto it = range.first; it != range.second; ++it)
            {
                auto const& suf = *it->second;
                if (p.score.hops.size() < 2 || suf.score.hops.size() < 2)
                    continue;
                auto const& a = p.score.hops.front();
                auto const& b = p.score.hops.back();
                auto const& m = suf.score.hops.front();
                if (a == m || b == m || a == dstAsset)
                    continue;
                consider({a, b, m, dstAsset});
            }
        }
    }

    // Longer leftover hops (5–8) from a capped BFS.
    if (maxHops >= 5)
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
                    consider(hops);
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
        if (path.empty() || path.size() > SearchBudget::kMaxPathLength)
            continue;
        consider(hopsFromPath(path));
    }

    // Incremental convert-all must see every unique 2-hop, including a
    // thin USD.GateHub AMM that destWidth-sort would drop for MAG/PROFIT.
    std::vector<PathScore> allShortHops = shortHops;

    auto takeBest = [&](std::vector<PathScore>& v) {
        std::ranges::sort(v, [&](PathScore const& a, PathScore const& b) {
            return betterScore(a, b, convertAll, minUseful);
        });
        if (static_cast<int>(v.size()) > budget.twoHop)
            v.resize(static_cast<std::size_t>(budget.twoHop));
    };
    takeBest(shortHops);
    takeBest(longHops);

    std::vector<PathScore> scored;
    scored.reserve(shortHops.size() + longHops.size());
    scored.insert(scored.end(), shortHops.begin(), shortHops.end());
    scored.insert(scored.end(), longHops.begin(), longHops.end());

    struct Cand
    {
        xrpl::STPath path;
        int scoreIdx{0};
        int bookHops{0};
    };
    std::vector<Cand> candidates;
    candidates.reserve(scored.size() * 2);
    auto emitPath = [&](xrpl::STPath path, int scoreIdx, int bookHops) {
        if (path.empty() || path.size() > SearchBudget::kMaxPathLength)
            return;
        if (candidates.size() >= 256)
            return;
        candidates.push_back(Cand{std::move(path), scoreIdx, bookHops});
    };
    for (int i = 0; i < static_cast<int>(scored.size()); ++i)
    {
        auto const hopsN = static_cast<int>(scored[static_cast<std::size_t>(i)].hops.size());
        auto booksOnly = bookPath(scored[static_cast<std::size_t>(i)].hops);
        auto bridged = issuerBridgePath(scored[static_cast<std::size_t>(i)].hops);
        bool const hasBridge = bridged.size() != booksOnly.size();
        if (!issuerIsSender)
        {
            auto prefixed = prefixSourceIssuer(booksOnly, srcAsset);
            if (prefixed.size() != booksOnly.size())
                emitPath(std::move(prefixed), i, hopsN);
        }
        emitPath(std::move(booksOnly), i, hopsN);
        if (hasBridge)
            emitPath(std::move(bridged), i, hopsN);
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
        xrpl::Quality quality{worstQuality()};
        xrpl::STAmount liquidity;
        std::size_t length{};
        int index{};
        bool fromCalc{false};
    };
    std::vector<Rank> ranks;

    int const limit = std::min(static_cast<int>(candidates.size()), budget.rank);
    std::unordered_set<std::string> rankedKeys;
    std::unordered_set<std::string> rankedHubs;
    rankedKeys.reserve(static_cast<std::size_t>(limit));
    rankedHubs.reserve(static_cast<std::size_t>(limit));

    auto pushCheap = [&](int i, xrpl::STPath const& path) {
        auto const scoreIdx = candidates[static_cast<std::size_t>(i)].scoreIdx;
        ranks.push_back(Rank{
            .quality = (static_cast<std::size_t>(scoreIdx) < scored.size())
                ? scored[static_cast<std::size_t>(scoreIdx)].quality
                : worstQuality(),
            .liquidity = (static_cast<std::size_t>(scoreIdx) < scored.size() &&
                          scored[static_cast<std::size_t>(scoreIdx)].destWidth > 0)
                ? xrpl::STAmount(
                      dstAsset,
                      static_cast<std::uint64_t>(
                          std::min(scored[static_cast<std::size_t>(scoreIdx)].destWidth, 1e15)),
                      0,
                      false)
                : xrpl::STAmount{dstAsset},
            .length = path.size(),
            .index = i,
            .fromCalc = false});
    };

    ranks.reserve(static_cast<std::size_t>(std::max(limit, SearchBudget::kMaxPathCount)));
    int attempts = 0;
    int successes = 0;
    int const need = SearchBudget::kMaxPathCount;
    int const attemptCap = std::min(
        static_cast<int>(candidates.size()), std::max(budget.twoHop, budget.rank));
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
    {
        if (continueCallback && !continueCallback())
            break;
        auto const& cand = candidates[static_cast<std::size_t>(i)];
        auto const& path = cand.path;
        if (path.size() > SearchBudget::kMaxPathLength)
            continue;
        bool const longHop = cand.bookHops > 2;
        // Convert-all with send_max must isolate more unique hubs so
        // incremental Flow can prefer USD.GateHub over a fat PROFIT tip.
        int const isolateNeed =
            (convertAll && sendIn > 0) ? 16 : need;
        int const isolateAttempts =
            (convertAll && sendIn > 0) ? std::max(budget.rank, 24) : budget.rank;
        // Have enough working 2-hops and already priced `rank` of them:
        // do not spend calcs on longer speculative hops.
        if (successes >= isolateNeed && (longHop || attempts >= isolateAttempts))
            break;
        if (attempts >= attemptCap)
            break;
        auto const sig = hopCurrencyKey(path);
        auto const hub = hopBookKey(path);
        if (rankedHubs.contains(hub))
            continue;
        if (!rankedKeys.insert(sig).second)
            continue;
        ++attempts;
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

            rankedHubs.insert(hub);
            ++successes;
            ranks.push_back(Rank{
                .quality = xrpl::Quality{xrpl::getRate(rc.actualAmountOut, rc.actualAmountIn)},
                .liquidity = rc.actualAmountOut,
                .length = path.size(),
                .index = i,
                .fromCalc = true});
        }
        catch (std::exception const& ex)
        {
            JLOG(j.debug()) << "fast path rank exception: " << ex.what();
        }
    }
    // Empty / no-liquidity ledgers (unit tests) still need the cheap order.
    if (ranks.empty())
    {
        rankedKeys.clear();
        for (int i = 0; i < static_cast<int>(candidates.size()) &&
             static_cast<int>(ranks.size()) < limit;
             ++i)
        {
            auto const& path = candidates[static_cast<std::size_t>(i)].path;
            if (path.size() > SearchBudget::kMaxPathLength)
                continue;
            if (!rankedKeys.insert(hopBookKey(path)).second)
                continue;
            pushCheap(i, path);
        }
    }

    std::ranges::sort(ranks, [&](Rank const& a, Rank const& b) {
        if (!convertAll && a.quality != b.quality)
            return a.quality > b.quality;
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
            auto const& cand = candidates[static_cast<std::size_t>(r.index)];
            auto const& path = cand.path;
            if (path.size() > SearchBudget::kMaxPathLength)
                continue;
            if (shortOnly && cand.bookHops > 2)
                continue;
            if (!shortOnly && cand.bookHops <= 2)
                continue;
            if (!chosen.insert(hopBookKey(path)).second)
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

    // Convert-all send_max is quoted by Pathfinder + one RippleCalc in
    // the session (xrpld PathRequest). Incremental Flow here replaced
    // the six with dest+XRP and burned hundreds of extra calcs.
    if (false && convertAll && sendIn > 0 && ledger)
    {
        auto flowOut = [&](xrpl::STPathSet const& ps) -> double {
            if (ps.empty())
                return 0;
            if (continueCallback && !continueCallback())
                return 0;
            try
            {
                xrpl::PaymentSandbox sandbox(&*ledger, xrpl::TapNone);
                xrpl::path::RippleCalc::Input in;
                in.defaultPathsAllowed = false;
                in.partialPaymentAllowed = true;
                auto rc = rippleCalculate(
                    sandbox,
                    saMax,
                    xrpl::largestAmount(dstAmount),
                    dst,
                    src,
                    ps,
                    domain,
                    services,
                    &in);
                if (!xrpl::isTesSuccess(rc.result()))
                    return 0;
                return amountAsDouble(rc.actualAmountOut);
            }
            catch (...)
            {
                return 0;
            }
        };
        struct HubCand
        {
            xrpl::STPath prefix;
            xrpl::STPath book;
        };
        std::unordered_map<std::string, HubCand> byHub;
        auto addHub = [&](xrpl::STPath p) {
            if (p.empty() || p.size() > SearchBudget::kMaxPathLength)
                return;
            auto& slot = byHub[hopBookKey(p)];
            if (!p.empty() && pathElementIsAccount(p.front()))
                slot.prefix = std::move(p);
            else if (slot.book.empty())
                slot.book = std::move(p);
        };
        for (auto const& s : allShortHops)
        {
            auto p = bookPath(s.hops);
            if (!issuerIsSender)
                addHub(prefixSourceIssuer(p, srcAsset));
            addHub(std::move(p));
        }
        for (auto const& r : ranks)
        {
            if (!r.fromCalc)
                continue;
            addHub(candidates[static_cast<std::size_t>(r.index)].path);
        }

        xrpl::STPath destHop = bookPath({dstAsset});
        xrpl::STPath xrpHop = bookPath({xrp, dstAsset});
        auto isRequired = [&](std::string const& key) {
            return key == hopBookKey(destHop) || key == hopBookKey(xrpHop);
        };
        // Start from the isolate six. Replacing that set with dest+XRP
        // alone (when no hub increments) is what dropped cards to 2 hops.
        xrpl::STPathSet grown = result.paths;
        if (grown.empty())
        {
            if (srcAsset != dstAsset && !xrpl::equalTokens(srcAsset, dstAsset))
                pathSetPushAlways(grown, destHop);
            if (!srcIsXrp && !dstIsXrp && srcAsset != dstAsset)
                pathSetPushAlways(grown, xrpHop);
        }
        double const isolateOut = flowOut(grown);
        double bestOut = isolateOut;
        auto grownHas = [&](std::string const& key) {
            for (auto const& p : grown)
            {
                if (hopBookKey(p) == key)
                    return true;
            }
            return false;
        };
        auto trialWith = [&](xrpl::STPath const& p) {
            xrpl::STPathSet trial;
            if (static_cast<int>(grown.size()) < SearchBudget::kMaxPathCount)
            {
                trial = grown;
                pathSetPushAlways(trial, p);
                return trial;
            }
            int drop = -1;
            for (int i = static_cast<int>(grown.size()) - 1; i >= 0; --i)
            {
                if (!isRequired(hopBookKey(grown[static_cast<std::size_t>(i)])))
                {
                    drop = i;
                    break;
                }
            }
            if (drop < 0)
                return trial;
            for (int i = 0; i < static_cast<int>(grown.size()); ++i)
            {
                if (i != drop)
                    pathSetPushAlways(trial, grown[static_cast<std::size_t>(i)]);
            }
            pathSetPushAlways(trial, p);
            return trial;
        };

        struct Tried
        {
            std::string key;
            xrpl::STPath path;
        };
        std::vector<Tried> live;
        live.reserve(byHub.size());
        for (auto& [key, slot] : byHub)
        {
            if (continueCallback && !continueCallback())
                break;
            if (grownHas(key))
                continue;
            xrpl::STPath bestP;
            double bestPOut = bestOut;
            auto tryP = [&](xrpl::STPath const& p) {
                if (p.empty())
                    return;
                auto const trial = trialWith(p);
                if (trial.empty())
                    return;
                double const out = flowOut(trial);
                if (out > bestPOut)
                {
                    bestPOut = out;
                    bestP = p;
                }
            };
            tryP(slot.book);
            if (bestP.empty())
                tryP(slot.prefix);
            if (!bestP.empty())
                live.push_back(Tried{key, std::move(bestP)});
        }

        while (!live.empty())
        {
            if (continueCallback && !continueCallback())
                break;
            int add = -1;
            double addOut = bestOut;
            xrpl::STPathSet addTrial;
            for (int i = 0; i < static_cast<int>(live.size()); ++i)
            {
                if (grownHas(live[static_cast<std::size_t>(i)].key))
                    continue;
                auto trial = trialWith(live[static_cast<std::size_t>(i)].path);
                if (trial.empty())
                    continue;
                double const out = flowOut(trial);
                if (out > addOut)
                {
                    addOut = out;
                    add = i;
                    addTrial = std::move(trial);
                }
            }
            if (add < 0)
                break;
            grown = std::move(addTrial);
            bestOut = addOut;
        }
        // Never shrink the isolate six. Only keep incremental if it
        // pays more dest and still has at least as many hops.
        if (bestOut > isolateOut &&
            static_cast<int>(grown.size()) >= static_cast<int>(result.paths.size()) &&
            static_cast<int>(grown.size()) >= 2)
            result.paths = std::move(grown);
    }

    // Flow must see dest and the XRP bridge even when isolate ranking
    // filled six 2-hops. Evict a non-required tail slot — do not evict
    // dest to make room for the bridge (or the other way around).
    std::vector<xrpl::STPath> required;
    if (srcAsset != dstAsset && !xrpl::equalTokens(srcAsset, dstAsset))
        required.push_back(bookPath({dstAsset}));
    if (!srcIsXrp && !dstIsXrp && srcAsset != dstAsset)
        required.push_back(bookPath({xrp, dstAsset}));
    auto hasKey = [](xrpl::STPathSet const& set, std::string const& key) {
        for (auto const& p : set)
        {
            if (hopBookKey(p) == key)
                return true;
        }
        return false;
    };
    auto requiredKey = [&](std::string const& key) {
        for (auto const& p : required)
        {
            if (hopBookKey(p) == key)
                return true;
        }
        return false;
    };
    for (auto const& path : required)
    {
        if (path.empty())
            continue;
        auto const key = hopBookKey(path);
        if (hasKey(result.paths, key))
            continue;
        if (static_cast<int>(result.paths.size()) >= SearchBudget::kMaxPathCount)
        {
            int drop = -1;
            for (int i = static_cast<int>(result.paths.size()) - 1; i >= 0; --i)
            {
                if (!requiredKey(hopBookKey(result.paths[static_cast<std::size_t>(i)])))
                {
                    drop = i;
                    break;
                }
            }
            if (drop < 0)
                continue;
            xrpl::STPathSet kept;
            for (int i = 0; i < static_cast<int>(result.paths.size()); ++i)
            {
                if (i != drop)
                    pathSetPushAlways(kept, result.paths[static_cast<std::size_t>(i)]);
            }
            result.paths = std::move(kept);
        }
        if (static_cast<int>(result.paths.size()) < SearchBudget::kMaxPathCount)
            pathSetPushAlways(result.paths, path);
    }
    for (auto const& r : ranks)
    {
        if (static_cast<int>(result.discovered.size()) >= budget.rank)
            break;
        auto const& path = candidates[static_cast<std::size_t>(r.index)].path;
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
