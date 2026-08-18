#include <edgy/session.hpp>

#include <edgy/book_util.hpp>
#include <edgy/compat.hpp>
#include <edgy/config.hpp>
#include <edgy/graph.hpp>
#include <edgy/protocol.hpp>
#include <edgy/ripple_calc.hpp>
#include <edgy/services.hpp>

#include <xrpld/rpc/detail/AccountAssets.h>
#include <xrpld/rpc/detail/PathfinderUtils.h>
#include <xrpld/rpc/detail/Tuning.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/json/json_value.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/ledger/PaymentSandbox.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/AmountConversions.h>
#include <xrpl/protocol/IOUAmount.h>
#include <xrpl/protocol/Quality.h>
#include <xrpl/protocol/XRPAmount.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/SystemParameters.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/tx/paths/RippleCalc.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <variant>

namespace edgy {

AutoSourcePick
pickAutoSources(
    std::vector<xrpl::PathAsset> const& ordered,
    xrpl::AccountID const& src,
    xrpl::Asset const& dstAsset,
    bool sameAccount,
    std::size_t maxSources)
{
    AutoSourcePick out;
    for (auto const& asset : ordered)
    {
        bool atCap = false;
        visitAsset(
            asset,
            [&](xrpl::Currency const& a) {
                if (!sameAccount || a != dstAsset)
                {
                    if (out.assets.size() >= maxSources)
                    {
                        atCap = true;
                        return;
                    }
                    out.assets.insert(
                        xrpl::Issue{a, a.isZero() ? xrpl::xrpAccount() : src});
                }
            },
            [&](xrpl::MPTID const& a) {
                if (!sameAccount || a != dstAsset)
                {
                    if (out.assets.size() >= maxSources)
                    {
                        atCap = true;
                        return;
                    }
                    out.assets.insert(xrpl::MPTIssue{a});
                }
            });
        if (atCap)
        {
            out.truncated = true;
            break;
        }
    }
    return out;
}

namespace {

constexpr int kPjInvalid = -1;
constexpr int kPjOk = 0;

enum PathWarn : int
{
    WarnPathLinesPartial = 2101,
    WarnPathRevalidateFailed = 2102,
    WarnPathSourceCurrenciesTruncated = 2103,
    WarnPathLinesBudget = 2104,
    WarnPathClobWalk = 2105,
};

xrpl::STPathSet
cappedPaths(xrpl::STPathSet const& paths)
{
    xrpl::STPathSet out;
    auto const n = std::min(
        paths.size(), static_cast<std::size_t>(xrpl::rpc::tuning::kPathFindMaxPaths));
    for (std::size_t i = 0; i < n; ++i)
        pathSetPushAlways(out, paths[i]);
    return out;
}

xrpl::STPath
directDestPath(xrpl::Asset const& dstAsset)
{
    xrpl::STPath path;
    pathPush(path, bookPathElement(dstAsset));
    return path;
}

xrpl::STPath
xrpBridgePath(xrpl::Asset const& dstAsset)
{
    xrpl::STPath path;
    pathPush(path, bookPathElement(xrpl::xrpIssue()));
    pathPush(path, bookPathElement(dstAsset));
    return path;
}

bool
pathIsDestOnly(xrpl::STPath const& path, xrpl::Asset const& dest)
{
    return path == directDestPath(dest);
}

bool
quoteBetter(
    xrpl::path::RippleCalc::Output const& a,
    xrpl::path::RippleCalc::Output const& b,
    bool convertAll)
{
    if (a.result() != xrpl::tesSUCCESS)
        return false;
    if (b.result() != xrpl::tesSUCCESS)
        return true;
    if (convertAll)
        return a.actualAmountOut > b.actualAmountOut;
    if (a.actualAmountIn != b.actualAmountIn)
        return a.actualAmountIn < b.actualAmountIn;
    return a.actualAmountOut > b.actualAmountOut;
}

struct PaidQuote
{
    xrpl::path::RippleCalc::Output rc;
    xrpl::STPathSet paths;
    bool ok{false};
    bool clobWalkFault{false};
};

PaidQuote
runPaidQuote(
    std::shared_ptr<xrpl::ReadView const> const& ledger,
    xrpl::STAmount const& saMax,
    xrpl::STAmount const& dstAmount,
    xrpl::AccountID const& dst,
    xrpl::AccountID const& src,
    xrpl::STPathSet const& paths,
    std::optional<xrpl::uint256> const& domain,
    PathServices& services,
    xrpl::path::RippleCalc::Input const& rcInput,
    xrpl::STPathSet const* displayPaths = nullptr)
{
    PaidQuote q;
    q.paths = displayPaths ? *displayPaths : paths;
    if (!ledger)
        return q;
    try
    {
        xrpl::PaymentSandbox sandbox(&*ledger, xrpl::TapNone);
        q.rc = rippleCalculate(
            sandbox, saMax, dstAmount, dst, src, paths, domain, services, &rcInput);
        q.ok = q.rc.result() == xrpl::tesSUCCESS;
    }
    catch (...)
    {
        q.ok = false;
    }
    return q;
}

void
keepIfBetter(PaidQuote& best, PaidQuote&& cand, bool convertAll)
{
    if (!cand.ok)
        return;
    if (!best.ok || quoteBetter(cand.rc, best.rc, convertAll))
        best = std::move(cand);
}

xrpl::path::RippleCalc::Input
withDefaultPaths(xrpl::path::RippleCalc::Input in, bool allow)
{
    in.defaultPathsAllowed = allow;
    return in;
}

xrpl::STPathSet
mergePaths(xrpl::STPathSet const& first, xrpl::STPathSet const& rest)
{
    xrpl::STPathSet out;
    auto add = [&](xrpl::STPath const& p) {
        if (p.empty())
            return;
        if (static_cast<int>(out.size()) >= SearchBudget::kMaxPathCount)
            return;
        pathSetPush(out, p);
    };
    for (auto const& p : first)
        add(p);
    for (auto const& p : rest)
        add(p);
    return out;
}

PaidQuote
bestPaidQuote(
    std::shared_ptr<xrpl::ReadView const> const& ledger,
    xrpl::STAmount const& saMax,
    xrpl::STAmount const& dstAmount,
    xrpl::AccountID const& dst,
    xrpl::AccountID const& src,
    xrpl::STPathSet const& found,
    std::optional<xrpl::uint256> const& domain,
    PathServices& services,
    xrpl::path::RippleCalc::Input const& rcInput,
    bool convertAll,
    bool isolateHops = true)
{
    // Isolate-rank dest, the XRP bridge, and each found hop so a junk
    // 4-hop set cannot hide RLUSD→XRP. Then Flow the working set
    // together — xrpld PathRequest does one rippleCalculate on the six
    // with default paths on. Quoting each hop alone walks one book for
    // the full dest and loses the tip of the other five (slightly worse
    // rate on most swaps).
    // Revalidate already has the six: one combined Flow is the Payment
    // quote. Per-hop RippleCalc of send_max was seconds on deep books.
    if (!isolateHops && !found.empty())
    {
        auto combined = runPaidQuote(
            ledger,
            saMax,
            dstAmount,
            dst,
            src,
            found,
            domain,
            services,
            withDefaultPaths(rcInput, true),
            &found);
        if (combined.ok)
            return combined;
    }
    PaidQuote isolated;
    auto const dest = dstAmount.asset();
    auto const srcAsset = saMax.asset();
    auto const destHop = directDestPath(dest);
    auto const noDefault = withDefaultPaths(rcInput, false);
    if (srcAsset != dest)
    {
        xrpl::STPathSet oneHop;
        pathSetPushAlways(oneHop, destHop);
        keepIfBetter(
            isolated,
            runPaidQuote(
                ledger, saMax, dstAmount, dst, src, oneHop, domain, services, noDefault),
            convertAll);
    }
    if (!xrpl::isXRP(srcAsset) && !xrpl::isXRP(dest) && srcAsset != dest)
    {
        xrpl::STPathSet onlyBridge;
        pathSetPushAlways(onlyBridge, xrpBridgePath(dest));
        keepIfBetter(
            isolated,
            runPaidQuote(
                ledger,
                saMax,
                dstAmount,
                dst,
                src,
                onlyBridge,
                domain,
                services,
                noDefault),
            convertAll);
    }
    for (auto const& p : found)
    {
        if (p.empty())
            continue;
        xrpl::STPathSet one;
        pathSetPushAlways(one, p);
        keepIfBetter(
            isolated,
            runPaidQuote(
                ledger, saMax, dstAmount, dst, src, one, domain, services, noDefault),
            convertAll);
    }

    if (found.empty())
        return isolated;

    auto combinedIn = withDefaultPaths(rcInput, true);
    auto combined = runPaidQuote(
        ledger,
        saMax,
        dstAmount,
        dst,
        src,
        found,
        domain,
        services,
        combinedIn,
        &found);
    bool const isolatedBetter = isolated.ok && combined.ok &&
        quoteBetter(isolated.rc, combined.rc, convertAll);
    if (pickPublishedQuote(isolated.ok, combined.ok, isolatedBetter) == QuotePick::Combined)
        return combined;
    return isolated;
}

// Convert-all dest: walk the dest CLOB and the dest+XRP CLOB. Combined
// Flow spends send_max on dest AMM (~8550); dest+XRP RippleCalc gulps
// the native AMM (~8710). The funded CLOB is ~8760 and is cheap to walk.
// Flow only if both walks miss (AMM-only books). paths_computed is the
// FastPathFinder six with the winning dest path first.
PaidQuote
quoteConvertAll(
    std::shared_ptr<xrpl::ReadView const> const& ledger,
    xrpl::STAmount const& saMax,
    xrpl::STAmount const& dstAmount,
    xrpl::AccountID const& dst,
    xrpl::AccountID const& src,
    xrpl::STPathSet const& found,
    std::optional<xrpl::uint256> const& domain,
    PathServices& services,
    xrpl::path::RippleCalc::Input const& rcInput)
{
    PaidQuote best;
    auto quoteAmt = [&](xrpl::STPathSet const& paths,
                        bool allowDefault,
                        xrpl::STAmount const& dest) {
        keepIfBetter(
            best,
            runPaidQuote(
                ledger,
                saMax,
                dest,
                dst,
                src,
                paths,
                domain,
                services,
                withDefaultPaths(rcInput, allowDefault)),
            true);
    };
    auto quote = [&](xrpl::STPathSet const& paths, bool allowDefault) {
        quoteAmt(paths, allowDefault, dstAmount);
    };
    if (saMax.asset() == dstAmount.asset())
    {
        if (!found.empty())
            quote(found, true);
        return best;
    }

    auto const destAsset = dstAmount.asset();
    // CLOB first. Convert-all RippleCalc walks the whole dest book and
    // was 16s+ on every WS revalidate of deep pairs (RLUSD/XAH). Dest
    // AMM is the worse fill on those books; skip Flow when a CLOB hit.
    if (ledger)
    {
        auto takeClob = [&](ClobWalkResult const& walk, xrpl::STPath const& path) {
            if (walk.ok())
            {
                PaidQuote clob;
                clob.ok = true;
                pathSetPushAlways(clob.paths, path);
                clob.rc.setResult(xrpl::tesSUCCESS);
                clob.rc.actualAmountIn = saMax;
                clob.rc.actualAmountOut = *walk.out;
                keepIfBetter(best, std::move(clob), true);
                return;
            }
            if (!clobWalkIsFault(walk))
                return;
            best.clobWalkFault = true;
            static std::mutex logMu;
            std::lock_guard const lock(logMu);
            std::cerr << "invariant clob_walk " << clobWalkWhyText(walk.why)
                      << " dirs=" << walk.dirs << " offers=" << walk.offers
                      << " taken=" << walk.taken
                      << " unfunded=" << walk.skippedUnfunded
                      << " skipped=" << walk.skippedOther << '\n';
        };
        takeClob(
            clobBookTake(*ledger, makeBook(saMax.asset(), destAsset, domain), saMax),
            directDestPath(destAsset));
        if (!xrpl::isXRP(saMax.asset()) && !xrpl::isXRP(destAsset))
        {
            takeClob(
                nativeBridgeClobOut(*ledger, saMax, destAsset, domain),
                xrpBridgePath(destAsset));
        }
    }
    if (best.ok)
        return best;

    if (!found.empty())
        quote(found, true);
    xrpl::STPathSet destOnly;
    pathSetPushAlways(destOnly, directDestPath(destAsset));
    quote(destOnly, false);
    if (xrpl::isXRP(saMax.asset()) || xrpl::isXRP(destAsset))
        return best;
    xrpl::STPathSet bridgeOnly;
    pathSetPushAlways(bridgeOnly, xrpBridgePath(destAsset));
    quote(bridgeOnly, false);
    return best;
}

void
setClosedLedgerIdentity(json::Value& dest, std::shared_ptr<xrpl::ReadView const> const& view)
{
    if (!view)
        return;
    if (!view->open())
    {
        dest[xrpl::jss::ledger_hash] = to_string(viewHeader(*view).hash);
        dest[xrpl::jss::ledger_index] = view->seq();
        return;
    }
    auto const& header = viewHeader(*view);
    if (header.seq > 0 && header.parentHash != beast::kZero)
    {
        dest[xrpl::jss::ledger_hash] = to_string(header.parentHash);
        dest[xrpl::jss::ledger_index] = header.seq - 1;
    }
}

void
setPathFindNotice(json::Value& dest, PathWarn code)
{
    if (dest.isMember(xrpl::jss::error) || dest.isMember(xrpl::jss::warning))
        return;

    char const* token = nullptr;
    char const* message = nullptr;
    switch (code)
    {
        case WarnPathLinesPartial:
            token = "path_lines_partial";
            message =
                "Trust lines for accounts used by this path_find subscription "
                "are still being filled. Results may be incomplete.";
            break;
        case WarnPathRevalidateFailed:
            token = "path_revalidate_failed";
            message =
                "Incremental revalidate found no live paths; the previous "
                "alternatives were re-sent for display only and may be stale.";
            break;
        case WarnPathSourceCurrenciesTruncated:
            token = "path_source_currencies_truncated";
            message =
                "The auto source-currency set was cut at the subscription "
                "soft cap. Results are valid for the included currencies only.";
            break;
        case WarnPathLinesBudget:
            token = "path_lines_budget";
            message =
                "The path cache line budget is exhausted. Accounts used by "
                "this request may be missing trust lines; empty alternatives "
                "do not mean no route exists.";
            break;
        case WarnPathClobWalk:
            token = "path_clob_walk";
            message =
                "The native-bridge CLOB walk returned no fill from a book "
                "that has offers (or threw). Dest may be stuck on the AMM floor.";
            break;
        default:
            return;
    }

    dest[xrpl::jss::warning] = token;
    if (!dest.isMember(xrpl::jss::warnings) || !dest[xrpl::jss::warnings].isArray())
        dest[xrpl::jss::warnings] = json::Value{kJsonArray};
    json::Value& w = dest[xrpl::jss::warnings].append(kJsonObject);
    w[xrpl::jss::id] = code;
    w[xrpl::jss::message] = message;
}

}  // namespace

PathSession::PathSession(
    PathServices& registry,
    Config const& cfg,
    int id,
    bool oneShot,
    beast::Journal journal)
    : registry_(registry), cfg_(cfg), journal_(journal), id_(id), oneShot_(oneShot)
{
    lastDepth_.store(cfg.searchFast, std::memory_order_relaxed);
}

bool
PathSession::shouldDeepen() const
{
    if (oneShot_ || closing_.load(std::memory_order_acquire))
        return false;
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - createdAt_);
    // Spread deepen so 100 sessions do not all jump a hop band together.
    age -= std::chrono::milliseconds{(id_ % 20) * 150};
    if (age.count() < 0)
        age = std::chrono::milliseconds{0};
    return SearchBudget::depthFor(false, 0, age, cfg_.searchFast, cfg_.search) >
        lastDepth_.load(std::memory_order_acquire);
}

bool
PathSession::shouldRediscover(xrpl::LedgerIndex ledgerSeq) const
{
    if (oneShot_ || closing_.load(std::memory_order_acquire))
        return false;
    // Still climbing hop bands — shouldDeepen owns those waves.
    if (lastDepth_.load(std::memory_order_acquire) < cfg_.search)
        return false;
    return rediscoveryDue(
        lastFullSearchIndex_,
        ledgerSeq,
        id_,
        xrpl::rpc::tuning::kPathFullSearchInterval);
}

int
PathSession::parseJson(json::Value const& jvIn)
{
    json::Value jvParams = jvIn;
    rewriteNativeJsonIn(jvParams, cfg_.network);

    if (!jvParams.isMember(xrpl::jss::source_account))
    {
        jvStatus_ = xrpl::rpcError(xrpl::RpcSrcActMissing);
        return kPjInvalid;
    }
    if (!jvParams.isMember(xrpl::jss::destination_account))
    {
        jvStatus_ = xrpl::rpcError(xrpl::RpcDstActMissing);
        return kPjInvalid;
    }
    if (!jvParams.isMember(xrpl::jss::destination_amount))
    {
        jvStatus_ = xrpl::rpcError(xrpl::RpcDstAmtMissing);
        return kPjInvalid;
    }

    src_ = xrpl::parseBase58<xrpl::AccountID>(jvParams[xrpl::jss::source_account].asString());
    if (!src_)
    {
        jvStatus_ = xrpl::rpcError(xrpl::RpcSrcActMalformed);
        return kPjInvalid;
    }
    dst_ = xrpl::parseBase58<xrpl::AccountID>(jvParams[xrpl::jss::destination_account].asString());
    if (!dst_)
    {
        jvStatus_ = xrpl::rpcError(xrpl::RpcDstActMalformed);
        return kPjInvalid;
    }
    if (!xrpl::amountFromJsonNoThrow(dstAmount_, jvParams[xrpl::jss::destination_amount]))
    {
        jvStatus_ = xrpl::rpcError(xrpl::RpcDstAmtMalformed);
        return kPjInvalid;
    }

    convertAll_ = dstAmount_ == xrpl::STAmount(dstAmount_.asset(), 1u, 0, true);
    if (!xrpl::validAsset(dstAmount_.asset()) || (!convertAll_ && dstAmount_ <= beast::kZero))
    {
        jvStatus_ = xrpl::rpcError(xrpl::RpcDstAmtMalformed);
        return kPjInvalid;
    }

    if (jvParams.isMember(xrpl::jss::send_max))
    {
        if (!convertAll_)
        {
            jvStatus_ = xrpl::rpcError(xrpl::RpcDstAmtMalformed);
            return kPjInvalid;
        }
        sendMax_.emplace();
        if (!xrpl::amountFromJsonNoThrow(*sendMax_, jvParams[xrpl::jss::send_max]) ||
            !xrpl::validAsset(sendMax_->asset()) ||
            (*sendMax_ <= beast::kZero &&
             *sendMax_ != xrpl::STAmount(sendMax_->asset(), 1u, 0, true)))
        {
            jvStatus_ = xrpl::rpcError(xrpl::RpcSendmaxMalformed);
            return kPjInvalid;
        }
    }

    if (jvParams.isMember(xrpl::jss::source_currencies))
    {
        json::Value const& jvSrcCurrencies = jvParams[xrpl::jss::source_currencies];
        if (!jvSrcCurrencies.isArray() || jvSrcCurrencies.size() == 0 ||
            jvSrcCurrencies.size() > xrpl::rpc::tuning::kMaxSrcCur)
        {
            jvStatus_ = xrpl::rpcError(xrpl::RpcSrcCurMalformed);
            return kPjInvalid;
        }
        sourceAssets_.clear();
        for (auto const& c : jvSrcCurrencies)
        {
            if (!xrpl::validJSONAsset(c) || !c.isObject())
            {
                jvStatus_ = xrpl::rpcError(xrpl::RpcSrcCurMalformed);
                return kPjInvalid;
            }
            xrpl::PathAsset srcPathAsset;
            if (c.isMember(xrpl::jss::currency))
            {
                xrpl::Currency currency;
                if (!c[xrpl::jss::currency].isString() ||
                    !xrpl::toCurrency(currency, c[xrpl::jss::currency].asString()))
                {
                    jvStatus_ = xrpl::rpcError(xrpl::RpcSrcCurMalformed);
                    return kPjInvalid;
                }
                srcPathAsset = currency;
            }
            else
            {
#ifdef EDGY_XAHAU
                jvStatus_ = xrpl::rpcError(xrpl::RpcSrcCurMalformed);
                return kPjInvalid;
#else
                xrpl::uint192 u;
                if (!c[xrpl::jss::mpt_issuance_id].isString() ||
                    !u.parseHex(c[xrpl::jss::mpt_issuance_id].asString()))
                {
                    jvStatus_ = xrpl::rpcError(xrpl::RpcSrcCurMalformed);
                    return kPjInvalid;
                }
                srcPathAsset = u;
#endif
            }

            xrpl::AccountID srcIssuerID;
            if (c.isMember(xrpl::jss::issuer) &&
                (c.isMember(xrpl::jss::mpt_issuance_id) || !c[xrpl::jss::issuer].isString() ||
                 !xrpl::toIssuer(srcIssuerID, c[xrpl::jss::issuer].asString())))
            {
                jvStatus_ = xrpl::rpcError(xrpl::RpcSrcIsrMalformed);
                return kPjInvalid;
            }

            if (srcPathAsset.holds<xrpl::Currency>())
            {
                if (srcPathAsset.get<xrpl::Currency>().isZero())
                {
                    if (srcIssuerID.isNonZero())
                    {
                        jvStatus_ = xrpl::rpcError(xrpl::RpcSrcCurMalformed);
                        return kPjInvalid;
                    }
                }
                else if (srcIssuerID.isZero())
                {
                    srcIssuerID = *src_;
                }
            }

            if (sendMax_)
            {
                if (srcPathAsset == sendMax_->asset())
                {
                    if (srcIssuerID != *src_ && sendMax_->getIssuer() != *src_ &&
                        srcIssuerID != sendMax_->getIssuer())
                    {
                        jvStatus_ = xrpl::rpcError(xrpl::RpcSrcIsrMalformed);
                        return kPjInvalid;
                    }
                    visitAsset(
                        srcPathAsset,
                        [&](xrpl::Currency const& currency) {
                            if (srcIssuerID != *src_)
                                sourceAssets_.insert(xrpl::Issue{currency, srcIssuerID});
                            else if (sendMax_->getIssuer() != *src_)
                                sourceAssets_.insert(
                                    xrpl::Issue{currency, sendMax_->getIssuer()});
                            sourceAssets_.insert(xrpl::Issue{currency, *src_});
                        },
                        [&](xrpl::MPTID const& mpt) { sourceAssets_.insert(mpt); });
                }
            }
            else
            {
                visitAsset(
                    srcPathAsset,
                    [&](xrpl::Currency const& currency) {
                        sourceAssets_.insert(xrpl::Issue{currency, srcIssuerID});
                    },
                    [&](xrpl::MPTID const& mpt) { sourceAssets_.insert(xrpl::MPTIssue{mpt}); });
            }
        }
    }

    if (jvParams.isMember(xrpl::jss::id))
        jvId_ = jvParams[xrpl::jss::id];

    if (jvParams.isMember(xrpl::jss::domain))
    {
        xrpl::uint256 num;
        if (!jvParams[xrpl::jss::domain].isString() ||
            !num.parseHex(jvParams[xrpl::jss::domain].asString()))
        {
            jvStatus_ = xrpl::rpcError(xrpl::RpcDomainMalformed);
            return kPjInvalid;
        }
        domain_ = num;
    }
    return kPjOk;
}

bool
PathSession::isValid(std::shared_ptr<xrpl::AssetCache> const& cache)
{
    if (!src_ || !dst_)
        return false;
    if (!convertAll_ && (sendMax_ || dstAmount_ <= beast::kZero))
    {
        jvStatus_ = xrpl::rpcError(xrpl::RpcDstAmtMalformed);
        return false;
    }
    auto const lrLedger = cache->getLedger();
    if (!lrLedger->exists(xrpl::keylet::account(*src_)))
    {
        jvStatus_ = xrpl::rpcError(xrpl::RpcSrcActNotFound);
        return false;
    }
    auto const sleDest = lrLedger->read(xrpl::keylet::account(*dst_));
    json::Value& jvDestCur = (jvStatus_[xrpl::jss::destination_currencies] = kJsonArray);
    if (!sleDest)
    {
        jvDestCur.append(json::Value(xrpl::systemCurrencyCode()));
        if (!dstAmount_.native())
        {
            jvStatus_ = xrpl::rpcError(xrpl::RpcActNotFound);
            return false;
        }
        if (!convertAll_ && dstAmount_ < xrpl::STAmount(lrLedger->fees().reserve))
        {
            jvStatus_ = xrpl::rpcError(xrpl::RpcDstAmtMalformed);
            return false;
        }
    }
    else
    {
        bool const disallowXRP(sleDest->isFlag(xrpl::lsfDisallowXRP));
        auto const destAssets = xrpl::accountDestAssets(*dst_, cache, !disallowXRP);
        for (auto const& asset : destAssets)
            jvDestCur.append(to_string(asset));
        jvStatus_[xrpl::jss::destination_tag] = (sleDest->getFlags() & xrpl::lsfRequireDestTag);
    }
    setClosedLedgerIdentity(jvStatus_, lrLedger);
    return true;
}

json::Value
emitNative(json::Value j, NetworkKind network)
{
    rewriteNativeJsonOut(j, network);
    return j;
}

std::pair<bool, json::Value>
PathSession::doCreate(std::shared_ptr<xrpl::AssetCache> const& cache, json::Value const& params)
{
    bool valid = false;
    if (parseJson(params) != kPjInvalid)
    {
        valid = isValid(cache);
        if (!oneShot_ && valid)
            return {valid, doUpdate(cache, false)};
    }
    return {valid, emitNative(jvStatus_, cfg_.network)};
}

json::Value
PathSession::doClose()
{
    closing_.store(true, std::memory_order_release);
    std::lock_guard const sl(lock_);
    jvStatus_[xrpl::jss::closed] = true;
    return emitNative(jvStatus_, cfg_.network);
}

json::Value
PathSession::doStatus()
{
    std::lock_guard const sl(lock_);
    jvStatus_[xrpl::jss::status] = xrpl::jss::success;
    return emitNative(jvStatus_, cfg_.network);
}

bool
PathSession::revalidatePaths(
    std::shared_ptr<xrpl::AssetCache> const& cache,
    xrpl::Asset const& asset,
    xrpl::STPathSet const& paths,
    xrpl::STAmount const& dstAmount,
    json::Value& jvArray,
    std::shared_ptr<xrpl::ReadView const> const& calcLedger)
{
    if (paths.empty() || !src_ || !dst_)
        return false;

    auto const& sourceAccount = [&] {
        if (!xrpl::isXRP(asset.getIssuer()))
            return asset.getIssuer();
        if (xrpl::isXRP(asset))
            return xrpl::xrpAccount();
        return *src_;
    }();

    xrpl::STAmount const saMaxAmount = [&]() {
        if (sendMax_)
            return *sendMax_;
        return visitAsset(
            asset,
            [&](xrpl::Issue const& issue) {
                return xrpl::STAmount(xrpl::Issue{issue.currency, sourceAccount}, 1u, 0, true);
            },
            [](xrpl::MPTIssue const& issue) { return xrpl::STAmount(issue, 1u, 0, true); });
    }();

    xrpl::path::RippleCalc::Input rcInput;
    if (convertAll_)
        rcInput.partialPaymentAllowed = true;
    else
        rcInput.defaultPathsAllowed = false;

    auto const ledger = calcLedger ? calcLedger : cache->getLedger();
    PaidQuote best;
    if (convertAll_)
    {
        // Same pick-best as the full search. Revalidate used to Flow
        // only the cached six and overwrite a dest+XRP 8750 with 8590.
        best = quoteConvertAll(
            ledger,
            saMaxAmount,
            dstAmount,
            *dst_,
            *src_,
            paths,
            domain_,
            registry_,
            rcInput);
        clobWalkFault_ = best.clobWalkFault;
        if (best.ok)
            context_[asset] = cappedPaths(mergePaths(best.paths, paths));
    }
    else
    {
        best = bestPaidQuote(
            ledger,
            saMaxAmount,
            dstAmount,
            *dst_,
            *src_,
            paths,
            domain_,
            registry_,
            rcInput,
            convertAll_,
            false);
        if (best.ok)
            context_[asset] = cappedPaths(mergePaths(best.paths, paths));
    }
    if (!best.ok)
        return false;

    json::Value jvEntry(kJsonObject);
    forceIssueAccount(best.rc.actualAmountIn, sourceAccount);
    jvEntry[xrpl::jss::source_amount] =
        best.rc.actualAmountIn.getJson(kJsonNone);
    jvEntry[xrpl::jss::paths_computed] =
        context_[asset].getJson(kJsonNone);
    if (convertAll_)
    {
        jvEntry[xrpl::jss::destination_amount] =
            best.rc.actualAmountOut.getJson(kJsonNone);
    }
    if (oneShot_)
        jvEntry[xrpl::jss::paths_canonical] = kJsonArray;
    jvArray.append(std::move(jvEntry));
    return true;
}

bool
PathSession::findPaths(
    std::shared_ptr<xrpl::AssetCache> const& cache,
    json::Value& jvArray,
    bool fullSearch,
    bool allowEscalate,
    bool forceFast,
    bool& didFullSearch,
    std::shared_ptr<xrpl::ReadView const> const& calcLedger,
    std::function<bool()> const& continueCallback)
{
    didFullSearch = false;
    auto sourceAssets = sourceAssets_;
    if (sourceAssets.empty() && sendMax_)
        sourceAssets.insert(sendMax_->asset());

    if (sourceAssets.empty())
    {
        auto const age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - createdAt_);
        auto const budget = SearchBudget::forDepth(
            forceFast ? cfg_.searchFast
                      : SearchBudget::depthFor(
                            oneShot_, searchesDone_, age, cfg_.searchFast, cfg_.search),
            oneShot_ ? 0 : exploreWave_);
        // Auto source set grows with subscription age. Explicit
        // source_currencies still honours kMaxSrcCur. More sendable
        // assets than the cap must truncate — never fail the request
        // (that was RpcInternal for any wallet with >18 lines).
        auto const hardMax = static_cast<std::size_t>(
            std::min(budget.autoSources, xrpl::rpc::tuning::kMaxSrcCur));
        auto assets = xrpl::accountSourceAssets(*src_, cache, true);
        bool const sameAccount = *src_ == *dst_;
        std::vector<xrpl::PathAsset> ordered;
        ordered.reserve(assets.size());
        for (auto const& asset : assets)
            ordered.push_back(asset);
        std::ranges::sort(ordered, [](xrpl::PathAsset const& a, xrpl::PathAsset const& b) {
            bool const aXrp = a.isXRP();
            bool const bXrp = b.isXRP();
            if (aXrp != bXrp)
                return aXrp;
            return to_string(a) < to_string(b);
        });

        auto picked =
            pickAutoSources(ordered, *src_, dstAmount_.asset(), sameAccount, hardMax);
        sourceAssets = std::move(picked.assets);
        sourceCurrenciesTruncated_ = picked.truncated;
    }
    else
    {
        sourceCurrenciesTruncated_ = false;
    }

    auto const dstAmount = xrpl::convertAmount(dstAmount_, convertAll_);

    if (!fullSearch)
    {
        bool anyOk = false;
        for (auto const& asset : sourceAssets)
        {
            auto it = context_.find(asset);
            if (it == context_.end() || it->second.empty())
                continue;
            if (revalidatePaths(cache, asset, it->second, dstAmount, jvArray, calcLedger))
                anyOk = true;
        }
        if (anyOk)
            return true;
        if (!allowEscalate)
            return true;
        fullSearch = true;
    }

    didFullSearch = true;
    auto const age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - createdAt_);
    auto const budget = SearchBudget::forDepth(
        forceFast ? cfg_.searchFast
                  : SearchBudget::depthFor(
                        oneShot_, searchesDone_, age, cfg_.searchFast, cfg_.search),
        oneShot_ ? 0 : exploreWave_);
    lastDepth_.store(budget.depth, std::memory_order_release);
    auto timedContinue = continueCallback;
    if (cfg_.searchTimeout.count() > 0)
    {
        auto const deadline = std::chrono::steady_clock::now() + cfg_.searchTimeout;
        timedContinue = [continueCallback, deadline] {
            if (continueCallback && !continueCallback())
                return false;
            return std::chrono::steady_clock::now() < deadline;
        };
    }
    auto& books = static_cast<PathServices&>(registry_).books();
    auto const ledger = calcLedger ? calcLedger : cache->getLedger();
    auto const tAll = std::chrono::steady_clock::now();
    std::vector<xrpl::Asset> orderedSources;
    orderedSources.reserve(sourceAssets.size());
    if (sendMax_)
    {
        auto const sendAsset = sendMax_->asset();
        if (sourceAssets.contains(sendAsset))
            orderedSources.push_back(sendAsset);
    }
    for (auto const& asset : sourceAssets)
    {
        if (std::ranges::find(orderedSources, asset) == orderedSources.end())
            orderedSources.push_back(asset);
    }
    std::vector<json::Value> withPaths;
    std::vector<json::Value> defaultOnly;
    for (auto const& asset : orderedSources)
    {
        if (timedContinue && !timedContinue())
            break;

        xrpl::STPathSet extra = context_[asset];
        if (auto pit = explorePool_.find(asset); pit != explorePool_.end())
        {
            for (auto const& p : pit->second)
                pathSetPush(extra, p);
        }

        // Convert-all dest is CLOB-walked in quoteConvertAll. Do not run
        // Pathfinder(7) + rank-every-complete-path here — that RippleCalc'd
        // up to 1000 full send_max fills and made WS revalidate 16s+.
        FastPathResult found = FastPathFinder::search(
            books,
            registry_,
            ledger,
            *src_,
            *dst_,
            asset,
            dstAmount,
            sendMax_,
            domain_,
            extra,
            convertAll_,
            budget,
            timedContinue);
        bool const usedFast = true;
        xrpl::STPathSet chosen = found.paths;
        auto remember = [&](xrpl::STPath const& p) {
            if (p.empty())
                return;
            auto& pool = explorePool_[asset];
            for (auto const& e : pool)
            {
                if (e == p)
                    return;
            }
            pool.push_front(p);
            constexpr std::size_t kExplorePoolCap = 24;
            if (pool.size() > kExplorePoolCap)
                pool.pop_back();
        };
        for (auto const& p : chosen)
            remember(p);
        if (usedFast)
        {
            for (auto const& p : found.paths)
                remember(p);
            for (auto const& p : found.discovered)
                remember(p);
        }
        auto ps = cappedPaths(chosen);
        context_[asset] = ps;

        if (usedFast)
        {
            auto const ms = found.search + found.rank;
            JLOG(journal_.info()) << "fast path_find depth=" << budget.depth
                                  << " candidates=" << found.candidates
                                  << " ranked=" << found.ranked << " kept=" << ps.size()
                                  << " search=" << found.search.count()
                                  << "ms rank=" << found.rank.count() << "ms";
            if (ms.count() >= 250)
            {
                std::cerr << "path_find depth=" << budget.depth << " " << found.candidates
                          << " candidates, " << ps.size() << " paths, " << ms.count()
                          << "ms (search " << found.search.count() << " rank "
                          << found.rank.count() << ")\n";
            }
        }

        auto const& sourceAccount = [&] {
            if (!xrpl::isXRP(asset.getIssuer()))
                return asset.getIssuer();
            if (xrpl::isXRP(asset))
                return xrpl::xrpAccount();
            return *src_;
        }();

        xrpl::STAmount const saMaxAmount = [&]() {
            if (sendMax_)
                return *sendMax_;
            return visitAsset(
                asset,
                [&](xrpl::Issue const& issue) {
                    return xrpl::STAmount(xrpl::Issue{issue.currency, sourceAccount}, 1u, 0, true);
                },
                [](xrpl::MPTIssue const& issue) { return xrpl::STAmount(issue, 1u, 0, true); });
        }();

        xrpl::path::RippleCalc::Input rcInput;
        if (convertAll_)
            rcInput.partialPaymentAllowed = true;
        else
            rcInput.defaultPathsAllowed = false;
        auto const tCalc = std::chrono::steady_clock::now();
        try
        {
            PaidQuote best;
            if (convertAll_)
            {
                best = quoteConvertAll(
                    ledger,
                    saMaxAmount,
                    dstAmount,
                    *dst_,
                    *src_,
                    ps,
                    domain_,
                    registry_,
                    rcInput);
                clobWalkFault_ = best.clobWalkFault;
                if (best.ok)
                    ps = cappedPaths(mergePaths(best.paths, ps));
            }
            else
            {
                best = bestPaidQuote(
                    ledger,
                    saMaxAmount,
                    dstAmount,
                    *dst_,
                    *src_,
                    ps,
                    domain_,
                    registry_,
                    rcInput,
                    convertAll_);
                if (best.ok)
                {
                    ps = cappedPaths(mergePaths(best.paths, found.paths));
                    if (static_cast<int>(ps.size()) < SearchBudget::kMaxPathCount)
                        ps = cappedPaths(mergePaths(ps, found.discovered));
                    if (auto pit = explorePool_.find(asset); pit != explorePool_.end())
                    {
                        xrpl::STPathSet pooled;
                        for (auto const& p : pit->second)
                            pathSetPush(pooled, p);
                        ps = cappedPaths(mergePaths(ps, pooled));
                    }
                }
            }
            auto const calcMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tCalc);

            if (best.ok)
            {
                context_[asset] = ps;
                json::Value jvEntry(kJsonObject);
                forceIssueAccount(best.rc.actualAmountIn, sourceAccount);
                jvEntry[xrpl::jss::source_amount] =
                    best.rc.actualAmountIn.getJson(kJsonNone);
                jvEntry[xrpl::jss::paths_computed] = ps.getJson(kJsonNone);
                if (convertAll_)
                {
                    jvEntry[xrpl::jss::destination_amount] =
                        best.rc.actualAmountOut.getJson(kJsonNone);
                }
                if (oneShot_)
                    jvEntry[xrpl::jss::paths_canonical] = kJsonArray;
                // UI clients (swap PathFind) only read alternatives[0]. Keep
                // computed hop lists ahead of default-path-only entries.
                if (ps.empty())
                    defaultOnly.push_back(std::move(jvEntry));
                else
                    withPaths.push_back(std::move(jvEntry));
            }
            JLOG(journal_.info()) << "fast path_find final calc " << calcMs.count()
                                  << "ms ok=" << best.ok;
        }
        catch (std::exception const& ex)
        {
            JLOG(journal_.warn()) << "fast path_find RippleCalc: " << ex.what();
        }
    }
    if (convertAll_ && withPaths.size() > 1)
    {
        std::ranges::stable_sort(
            withPaths, [](json::Value const& a, json::Value const& b) {
                auto destVal = [](json::Value const& e) -> double {
                    if (!e.isMember(xrpl::jss::destination_amount))
                        return 0;
                    auto const& da = e[xrpl::jss::destination_amount];
                    try
                    {
                        if (da.isObject() && da.isMember(xrpl::jss::value))
                            return std::stod(da[xrpl::jss::value].asString());
                        if (da.isString())
                            return std::stod(da.asString());
                    }
                    catch (...)
                    {
                    }
                    return 0;
                };
                return destVal(a) > destVal(b);
            });
    }
    for (auto& e : withPaths)
        jvArray.append(std::move(e));
    for (auto& e : defaultOnly)
        jvArray.append(std::move(e));
    auto const totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tAll);
    if (totalMs.count() >= 250)
    {
        std::cerr << "path_find depth=" << budget.depth << " total " << totalMs.count()
                  << "ms sources=" << sourceAssets.size() << " alts=" << jvArray.size() << '\n';
    }
    ++searchesDone_;
    return true;
}

json::Value
PathSession::doUpdate(
    std::shared_ptr<xrpl::AssetCache> const& cache,
    bool fast,
    bool revalidateOnly,
    std::shared_ptr<xrpl::ReadView const> const& calcLedger)
{
    xrpl::AssetCache::SearchPin const searchPin{*cache};
    xrpl::AssetCache::SessionPin const sessionPin{id_};
    std::optional<xrpl::AssetCache::LoadScope> lineLoadScope;
    if (oneShot_)
    {
        auto const room = cache->remainingBudget();
        if (room > 0)
            lineLoadScope.emplace(std::min(cfg_.maxLinesPerAccount, room));
    }

    {
        std::lock_guard const sl(lock_);
        if (!isValid(cache))
            return jvStatus_;
    }

    json::Value newStatus = kJsonObject;
    if (oneShot_)
    {
        (void)cache->getRippleLines(*dst_);
        while (cache->expandIncompleteLinesForSession(id_))
        {
        }
        auto& destAssets = (newStatus[xrpl::jss::destination_currencies] = kJsonArray);
        auto const assets = xrpl::accountDestAssets(*dst_, cache, true);
        for (auto const& asset : assets)
            destAssets.append(to_string(asset));
    }

    newStatus[xrpl::jss::source_account] = xrpl::toBase58(*src_);
    newStatus[xrpl::jss::destination_account] = xrpl::toBase58(*dst_);
    newStatus[xrpl::jss::destination_amount] =
        dstAmount_.getJson(kJsonNone);
    newStatus[xrpl::jss::full_reply] = false;
    if (jvId_)
        newStatus[xrpl::jss::id] = jvId_;

    bool const isSubscription = !oneShot_;

    auto const ledgerForSeq = calcLedger ? calcLedger : cache->getLedger();
    auto const ledgerSeq = ledgerForSeq->seq();
    auto const identityLedger =
        (ledgerForSeq && !ledgerForSeq->open()) ? ledgerForSeq : cache->getLedger();
    setClosedLedgerIdentity(newStatus, identityLedger);

    if (!revalidateOnly)
    {
        while (cache->expandIncompleteLinesForSession(id_))
        {
        }
    }

    bool const fullSearch = !revalidateOnly;
    // Dead context_ paths must run FastPathFinder again. Leaving
    // allowEscalate false replayed the last quote for the life of the
    // socket (path_revalidate_failed) while the market moved.
    bool const allowEscalate = true;

    json::Value jvArray = kJsonArray;
    bool didFullSearch = false;
    if (findPaths(
            cache,
            jvArray,
            fullSearch,
            allowEscalate,
            fast,
            didFullSearch,
            ledgerForSeq,
            {}))
    {
        bool restoredStale = false;
        if (jvArray.size() == 0 && isSubscription && !didFullSearch)
        {
            std::lock_guard const sl(lock_);
            if (jvStatus_.isMember(xrpl::jss::alternatives) &&
                jvStatus_[xrpl::jss::alternatives].size() > 0)
            {
                jvArray = jvStatus_[xrpl::jss::alternatives];
                restoredStale = true;
            }
        }
        if (restoredStale)
        {
            lastSuccess_ = false;
            newStatus[xrpl::jss::full_reply] = false;
            setPathFindNotice(newStatus, WarnPathRevalidateFailed);
        }
        else
        {
            lastSuccess_ = jvArray.size() != 0;
            // xrpld marks a quote complete once the search is done, including
            // ledger reprices. We used to set full_reply only on FastPathFinder
            // waves, so UIs that commit the rate on full_reply froze after the
            // last deepen (search-fast → search) for the rest of the socket.
            auto const atTarget =
                lastDepth_.load(std::memory_order_acquire) >= cfg_.search;
            newStatus[xrpl::jss::full_reply] =
                didFullSearch || (atTarget && lastSuccess_);
        }
        if (didFullSearch)
        {
            lastFullSearchIndex_ = ledgerSeq;
            lastLineEpoch_ = cache->lineEpoch();
            if (!oneShot_)
                ++exploreWave_;
        }
        newStatus[xrpl::jss::alternatives] = std::move(jvArray);
    }
    else
    {
        lastSuccess_ = false;
        if (didFullSearch)
        {
            lastFullSearchIndex_ = ledgerSeq;
            lastLineEpoch_ = cache->lineEpoch();
            if (!oneShot_)
                ++exploreWave_;
        }
        newStatus = xrpl::rpcError(xrpl::RpcInternal);
    }

    if (sourceCurrenciesTruncated_)
        setPathFindNotice(newStatus, WarnPathSourceCurrenciesTruncated);
    else if (cache->overBudget() && cache->hasIncompleteLinesForSession(id_))
        setPathFindNotice(newStatus, WarnPathLinesBudget);
    else if (cache->hasIncompleteLinesForSession(id_))
        setPathFindNotice(newStatus, WarnPathLinesPartial);
    else if (clobWalkFault_)
        setPathFindNotice(newStatus, WarnPathClobWalk);

    {
        std::lock_guard const sl(lock_);
        jvStatus_ = newStatus;
    }
    return emitNative(std::move(newStatus), cfg_.network);
}

}  // namespace edgy
