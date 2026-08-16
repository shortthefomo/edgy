#include <pathfinder/session.hpp>

#include <pathfinder/config.hpp>
#include <pathfinder/graph.hpp>
#include <pathfinder/services.hpp>

#include <xrpld/rpc/detail/AccountAssets.h>
#include <xrpld/rpc/detail/PathfinderUtils.h>
#include <xrpld/rpc/detail/Tuning.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/json/json_value.h>
#include <xrpl/ledger/PaymentSandbox.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/SystemParameters.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/server/LoadFeeTrack.h>
#include <xrpl/tx/paths/RippleCalc.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <variant>

namespace pathfinder {
namespace {

constexpr int kPjInvalid = -1;
constexpr int kPjOk = 0;

enum PathWarn : int
{
    WarnPathLinesPartial = 2101,
    WarnPathRevalidateFailed = 2102,
    WarnPathSourceCurrenciesTruncated = 2103,
    WarnPathLinesBudget = 2104,
};

xrpl::STPathSet
cappedPaths(xrpl::STPathSet const& paths)
{
    xrpl::STPathSet out;
    auto const n = std::min(
        paths.size(), static_cast<std::size_t>(xrpl::rpc::tuning::kPathFindMaxPaths));
    for (std::size_t i = 0; i < n; ++i)
        out.pushBack(paths[i]);
    return out;
}

void
setClosedLedgerIdentity(json::Value& dest, std::shared_ptr<xrpl::ReadView const> const& view)
{
    if (!view)
        return;
    if (!view->open())
    {
        dest[xrpl::jss::ledger_hash] = to_string(view->header().hash);
        dest[xrpl::jss::ledger_index] = view->seq();
        return;
    }
    auto const& header = view->header();
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
        default:
            return;
    }

    dest[xrpl::jss::warning] = token;
    if (!dest.isMember(xrpl::jss::warnings) || !dest[xrpl::jss::warnings].isArray())
        dest[xrpl::jss::warnings] = json::Value{json::ValueType::Array};
    json::Value& w = dest[xrpl::jss::warnings].append(json::ValueType::Object);
    w[xrpl::jss::id] = code;
    w[xrpl::jss::message] = message;
}

}  // namespace

PathSession::PathSession(
    xrpl::ServiceRegistry& registry,
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

int
PathSession::parseJson(json::Value const& jvParams)
{
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
                xrpl::uint192 u;
                if (!c[xrpl::jss::mpt_issuance_id].isString() ||
                    !u.parseHex(c[xrpl::jss::mpt_issuance_id].asString()))
                {
                    jvStatus_ = xrpl::rpcError(xrpl::RpcSrcCurMalformed);
                    return kPjInvalid;
                }
                srcPathAsset = u;
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
                    srcPathAsset.visit(
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
                srcPathAsset.visit(
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
    json::Value& jvDestCur = (jvStatus_[xrpl::jss::destination_currencies] = json::ValueType::Array);
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

std::pair<bool, json::Value>
PathSession::doCreate(std::shared_ptr<xrpl::AssetCache> const& cache, json::Value const& params)
{
    bool valid = false;
    if (parseJson(params) != kPjInvalid)
    {
        valid = isValid(cache);
        if (!oneShot_ && valid)
            jvStatus_ = doUpdate(cache, false);
    }
    return {valid, jvStatus_};
}

json::Value
PathSession::doClose()
{
    closing_.store(true, std::memory_order_release);
    std::lock_guard const sl(lock_);
    jvStatus_[xrpl::jss::closed] = true;
    return jvStatus_;
}

json::Value
PathSession::doStatus()
{
    std::lock_guard const sl(lock_);
    jvStatus_[xrpl::jss::status] = xrpl::jss::success;
    return jvStatus_;
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
        return asset.visit(
            [&](xrpl::Issue const& issue) {
                return xrpl::STAmount(xrpl::Issue{issue.currency, sourceAccount}, 1u, 0, true);
            },
            [](xrpl::MPTIssue const& issue) { return xrpl::STAmount(issue, 1u, 0, true); });
    }();

    xrpl::path::RippleCalc::Input rcInput;
    rcInput.defaultPathsAllowed = false;
    if (convertAll_)
        rcInput.partialPaymentAllowed = true;

    auto const ledger = calcLedger ? calcLedger : cache->getLedger();
    xrpl::path::RippleCalc::Output rc;
    try
    {
        xrpl::PaymentSandbox sandbox(&*ledger, xrpl::TapNone);
        rc = xrpl::path::RippleCalc::rippleCalculate(
            sandbox,
            saMaxAmount,
            dstAmount,
            *dst_,
            *src_,
            paths,
            domain_,
            registry_,
            &rcInput);
    }
    catch (std::exception const& ex)
    {
        JLOG(journal_.warn()) << "revalidate RippleCalc: " << ex.what();
        return false;
    }

    if (rc.result() != xrpl::tesSUCCESS)
        return false;

    json::Value jvEntry(json::ValueType::Object);
    if (rc.actualAmountIn.holds<xrpl::Issue>())
        rc.actualAmountIn.get<xrpl::Issue>().account = sourceAccount;
    jvEntry[xrpl::jss::source_amount] =
        rc.actualAmountIn.getJson(xrpl::JsonOptions::Values::None);
    jvEntry[xrpl::jss::paths_computed] =
        cappedPaths(paths).getJson(xrpl::JsonOptions::Values::None);
    if (convertAll_)
    {
        jvEntry[xrpl::jss::destination_amount] =
            rc.actualAmountOut.getJson(xrpl::JsonOptions::Values::None);
    }
    if (oneShot_)
        jvEntry[xrpl::jss::paths_canonical] = json::ValueType::Array;
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
                            oneShot_, searchesDone_, age, cfg_.searchFast, cfg_.search));
        // Auto source set grows with subscription age. Explicit
        // source_currencies still honours kMaxSrcCur.
        auto const hardMax = static_cast<std::size_t>(
            std::min(budget.autoSources, xrpl::rpc::tuning::kMaxSrcCur));
        std::size_t const softMax = hardMax;
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

        bool softTruncated = false;
        for (auto const& asset : ordered)
        {
            bool overHard = false;
            bool atSoft = false;
            std::visit(
                [&]<typename TAsset>(TAsset const& a) {
                    if (!sameAccount || a != dstAmount_.asset())
                    {
                        if (sourceAssets.size() >= hardMax)
                        {
                            overHard = true;
                            return;
                        }
                        if (sourceAssets.size() >= softMax)
                        {
                            atSoft = true;
                            return;
                        }
                        if constexpr (std::is_same_v<TAsset, xrpl::Currency>)
                        {
                            sourceAssets.insert(
                                xrpl::Issue{a, a.isZero() ? xrpl::xrpAccount() : *src_});
                        }
                        else
                        {
                            sourceAssets.insert(xrpl::MPTIssue{a});
                        }
                    }
                },
                asset.value());
            if (overHard)
                return false;
            if (atSoft)
            {
                softTruncated = true;
                break;
            }
        }
        sourceCurrenciesTruncated_ = softTruncated;
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
                        oneShot_, searchesDone_, age, cfg_.searchFast, cfg_.search));
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

        auto const found = FastPathFinder::search(
            books,
            registry_,
            ledger,
            *src_,
            *dst_,
            asset,
            dstAmount,
            sendMax_,
            domain_,
            context_[asset],
            convertAll_,
            budget,
            timedContinue);
        auto ps = cappedPaths(found.paths);
        context_[asset] = ps;

        auto const ms = found.search + found.rank;
        JLOG(journal_.info()) << "fast path_find depth=" << budget.depth
                              << " candidates=" << found.candidates
                              << " ranked=" << found.ranked << " kept=" << ps.size()
                              << " search=" << found.search.count()
                              << "ms rank=" << found.rank.count() << "ms";
        if (ms.count() >= 50)
        {
            std::cerr << "path_find depth=" << budget.depth << " " << found.candidates
                      << " candidates, " << ps.size() << " paths, " << ms.count()
                      << "ms (search " << found.search.count() << " rank "
                      << found.rank.count() << ")\n";
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
            return asset.visit(
                [&](xrpl::Issue const& issue) {
                    return xrpl::STAmount(xrpl::Issue{issue.currency, sourceAccount}, 1u, 0, true);
                },
                [](xrpl::MPTIssue const& issue) { return xrpl::STAmount(issue, 1u, 0, true); });
        }();

        xrpl::path::RippleCalc::Input rcInput;
        if (convertAll_)
            rcInput.partialPaymentAllowed = true;
        auto const tCalc = std::chrono::steady_clock::now();
        try
        {
            xrpl::PaymentSandbox sandbox(&*ledger, xrpl::TapNone);
            auto rc = xrpl::path::RippleCalc::rippleCalculate(
                sandbox, saMaxAmount, dstAmount, *dst_, *src_, ps, domain_, registry_, &rcInput);
            auto const calcMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tCalc);

            if (rc.result() == xrpl::tesSUCCESS)
            {
                json::Value jvEntry(json::ValueType::Object);
                if (rc.actualAmountIn.holds<xrpl::Issue>())
                    rc.actualAmountIn.get<xrpl::Issue>().account = sourceAccount;
                jvEntry[xrpl::jss::source_amount] =
                    rc.actualAmountIn.getJson(xrpl::JsonOptions::Values::None);
                jvEntry[xrpl::jss::paths_computed] = ps.getJson(xrpl::JsonOptions::Values::None);
                if (convertAll_)
                {
                    jvEntry[xrpl::jss::destination_amount] =
                        rc.actualAmountOut.getJson(xrpl::JsonOptions::Values::None);
                }
                if (oneShot_)
                    jvEntry[xrpl::jss::paths_canonical] = json::ValueType::Array;
                // UI clients (swap PathFind) only read alternatives[0]. Keep
                // computed hop lists ahead of default-path-only entries.
                if (ps.empty())
                    defaultOnly.push_back(std::move(jvEntry));
                else
                    withPaths.push_back(std::move(jvEntry));
            }
            JLOG(journal_.info()) << "fast path_find final calc " << calcMs.count()
                                  << "ms result=" << rc.result();
        }
        catch (std::exception const& ex)
        {
            JLOG(journal_.warn()) << "fast path_find RippleCalc: " << ex.what();
        }
    }
    for (auto& e : withPaths)
        jvArray.append(std::move(e));
    for (auto& e : defaultOnly)
        jvArray.append(std::move(e));
    auto const totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tAll);
    if (totalMs.count() >= 50)
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

    json::Value newStatus = json::ValueType::Object;
    if (oneShot_)
    {
        (void)cache->getRippleLines(*dst_);
        while (cache->expandIncompleteLinesForSession(id_))
        {
        }
        auto& destAssets = (newStatus[xrpl::jss::destination_currencies] = json::ValueType::Array);
        auto const assets = xrpl::accountDestAssets(*dst_, cache, true);
        for (auto const& asset : assets)
            destAssets.append(to_string(asset));
    }

    newStatus[xrpl::jss::source_account] = xrpl::toBase58(*src_);
    newStatus[xrpl::jss::destination_account] = xrpl::toBase58(*dst_);
    newStatus[xrpl::jss::destination_amount] =
        dstAmount_.getJson(xrpl::JsonOptions::Values::None);
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
    bool const allowEscalate = !revalidateOnly;

    json::Value jvArray = json::ValueType::Array;
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
        newStatus[xrpl::jss::full_reply] = didFullSearch;
        if (restoredStale)
        {
            lastSuccess_ = false;
            newStatus[xrpl::jss::full_reply] = false;
            setPathFindNotice(newStatus, WarnPathRevalidateFailed);
        }
        else
        {
            lastSuccess_ = jvArray.size() != 0;
        }
        if (didFullSearch)
        {
            lastFullSearchIndex_ = ledgerSeq;
            lastLineEpoch_ = cache->lineEpoch();
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
        }
        newStatus = xrpl::rpcError(xrpl::RpcInternal);
    }

    if (sourceCurrenciesTruncated_)
        setPathFindNotice(newStatus, WarnPathSourceCurrenciesTruncated);
    else if (cache->overBudget() && cache->hasIncompleteLinesForSession(id_))
        setPathFindNotice(newStatus, WarnPathLinesBudget);
    else if (cache->hasIncompleteLinesForSession(id_))
        setPathFindNotice(newStatus, WarnPathLinesPartial);

    {
        std::lock_guard const sl(lock_);
        jvStatus_ = newStatus;
    }
    return newStatus;
}

}  // namespace pathfinder
