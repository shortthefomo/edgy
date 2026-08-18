#pragma once

// Shared rippled / xahaud API adapters. xahaud TUs also get
// -DEDGY_XAHAU -Dxrpl=ripple so xrpl:: names resolve to ripple::.

#include <xrpl/beast/hash/uhash.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/json/json_value.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/Asset.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STPathSet.h>
#include <xrpl/protocol/SystemParameters.h>

#include <memory>
#include <string>
#include <utility>

#ifdef EDGY_XAHAU
#ifndef EDGY_JSON_IS_JSON
#define EDGY_JSON_IS_JSON
namespace json = Json;
#endif
namespace beast {
inline constexpr Zero const& kZero = zero;
template <class = void>
using Uhash = uhash<>;
}  // namespace beast
#endif

namespace edgy {

inline void
forceIssueAccount(xrpl::STAmount& amt, xrpl::AccountID const& account)
{
#ifdef EDGY_XAHAU
    if (!amt.holds<xrpl::Issue>())
        return;
    auto issue = amt.get<xrpl::Issue>();
    issue.account = account;
    amt = xrpl::STAmount(issue, amt.mantissa(), amt.exponent(), amt.negative());
#else
    if (amt.holds<xrpl::Issue>())
        amt.get<xrpl::Issue>().account = account;
#endif
}

#ifdef EDGY_XAHAU
inline constexpr auto kJsonObject = Json::objectValue;
inline constexpr auto kJsonArray = Json::arrayValue;
inline constexpr auto kJsonNone = xrpl::JsonOptions::none;
inline constexpr auto kPathTypeCurrency = xrpl::STPathElement::typeCurrency;
inline constexpr auto kPathTypeIssuer = xrpl::STPathElement::typeIssuer;
inline constexpr auto kPathTypeAccount = xrpl::STPathElement::typeAccount;
#else
inline constexpr auto kJsonObject = json::ValueType::Object;
inline constexpr auto kJsonArray = json::ValueType::Array;
inline constexpr auto kJsonNone = xrpl::JsonOptions::Values::None;
inline constexpr auto kPathTypeCurrency = xrpl::STPathElement::TypeCurrency;
inline constexpr auto kPathTypeIssuer = xrpl::STPathElement::TypeIssuer;
inline constexpr auto kPathTypeAccount = xrpl::STPathElement::TypeAccount;
#endif

[[nodiscard]] inline auto const&
viewHeader(xrpl::ReadView const& view)
{
#ifdef EDGY_XAHAU
    return view.info();
#else
    return view.header();
#endif
}

[[nodiscard]] inline xrpl::STPathElement
bookPathElement(xrpl::Asset const& out)
{
#ifdef EDGY_XAHAU
    if (xrpl::isXRP(out))
    {
        return xrpl::STPathElement(
            kPathTypeCurrency,
            xrpl::xrpAccount(),
            xrpl::xrpCurrency(),
            xrpl::xrpAccount());
    }
    auto const issue = out.holds<xrpl::Issue>() ? out.get<xrpl::Issue>() : xrpl::xrpIssue();
    return xrpl::STPathElement(
        kPathTypeCurrency | kPathTypeIssuer,
        xrpl::xrpAccount(),
        issue.currency,
        issue.account);
#else
    if (xrpl::isXRP(out))
    {
        return xrpl::STPathElement(
            xrpl::STPathElement::TypeCurrency,
            xrpl::xrpAccount(),
            xrpl::xrpCurrency(),
            xrpl::xrpAccount());
    }
    auto const assetType =
        out.holds<xrpl::Issue>() ? xrpl::STPathElement::TypeCurrency : xrpl::STPathElement::TypeMpt;
    return xrpl::STPathElement(
        assetType | xrpl::STPathElement::TypeIssuer,
        xrpl::xrpAccount(),
        out,
        out.getIssuer());
#endif
}

// Pathfinder book → issuer-account → book. After an IOU book hop, ripple
// through that issuer so the next book/AMM can consume the IOU (Gift→BITx
// AMM, then rBitcoi, then BITx/XRP).
[[nodiscard]] inline xrpl::STPathElement
accountPathElement(xrpl::Asset const& out)
{
#ifdef EDGY_XAHAU
    auto const issue = out.holds<xrpl::Issue>() ? out.get<xrpl::Issue>() : xrpl::xrpIssue();
    return xrpl::STPathElement(
        kPathTypeAccount, issue.account, issue.currency, issue.account);
#else
    return xrpl::STPathElement(
        xrpl::STPathElement::TypeAccount, out.getIssuer(), out, out.getIssuer());
#endif
}

[[nodiscard]] inline bool
pathElementIsAccount(xrpl::STPathElement const& el)
{
    return el.isAccount();
}

[[nodiscard]] inline bool
canIssuerHop(xrpl::Asset const& asset)
{
    if (xrpl::isXRP(asset))
        return false;
#ifdef EDGY_XAHAU
    return asset.holds<xrpl::Issue>() &&
        asset.get<xrpl::Issue>().account != xrpl::xrpAccount();
#else
    return asset.holds<xrpl::Issue>() && !xrpl::isXRP(asset.getIssuer());
#endif
}

inline void
pathPush(xrpl::STPath& path, xrpl::STPathElement const& el)
{
#ifdef EDGY_XAHAU
    path.push_back(el);
#else
    path.pushBack(el);
#endif
}

inline void
pathSetPush(xrpl::STPathSet& set, xrpl::STPath path)
{
#ifdef EDGY_XAHAU
    for (auto const& existing : set)
    {
        if (existing == path)
            return;
    }
    set.push_back(std::move(path));
#else
    if (set.contains(path))
        return;
    set.pushBack(std::move(path));
#endif
}

inline void
pathSetPushAlways(xrpl::STPathSet& set, xrpl::STPath const& path)
{
#ifdef EDGY_XAHAU
    set.push_back(path);
#else
    set.pushBack(path);
#endif
}

[[nodiscard]] inline std::string
pathElementKey(xrpl::STPathElement const& el)
{
#ifdef EDGY_XAHAU
    // Currency alone collapses USD.GateHub and USD.Bitstamp into one slot.
    std::string key = to_string(el.getCurrency());
    if (el.hasIssuer() && el.getIssuerID() != xrpl::xrpAccount())
    {
        key += '.';
        key += to_string(el.getIssuerID());
    }
    return key;
#else
    return to_string(el.getPathAsset());
#endif
}

// Pathfinder source hop: account + currency + issuer (rMx / RLUSD / rMx).
// TypeAccount-only is for mid-issuer rippling after a book, not this.
[[nodiscard]] inline xrpl::STPathElement
sourceIssuerPathElement(xrpl::Asset const& srcAsset)
{
#ifdef EDGY_XAHAU
    auto const issue =
        srcAsset.holds<xrpl::Issue>() ? srcAsset.get<xrpl::Issue>() : xrpl::xrpIssue();
    return xrpl::STPathElement(issue.account, issue.currency, issue.account);
#else
    return xrpl::STPathElement(srcAsset.getIssuer(), srcAsset, srcAsset.getIssuer());
#endif
}

// Currency sequence only — same hops / different issuers look identical
// in the UI and should not consume six path slots.
[[nodiscard]] inline std::string
hopCurrencyKey(xrpl::STPath const& path)
{
    std::string key;
    key.reserve(path.size() * 16);
    for (auto const& el : path)
    {
        if (pathElementIsAccount(el))
            key += "a:";
        key += pathElementKey(el);
        key.push_back('/');
    }
    return key;
}

// Book hops only. Issuer-account inserts must not take a second slot of
// the six (EVR and EVR→issuer→XAH crowded out USD/USDC on RLUSD→XAH).
[[nodiscard]] inline std::string
hopBookKey(xrpl::STPath const& path)
{
    std::string key;
    key.reserve(path.size() * 16);
    for (auto const& el : path)
    {
        if (pathElementIsAccount(el))
            continue;
        key += pathElementKey(el);
        key.push_back('/');
    }
    return key;
}

}  // namespace edgy
