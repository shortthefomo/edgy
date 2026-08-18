#pragma once

#include <edgy/services.hpp>

#include <xrpl/ledger/PaymentSandbox.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STPathSet.h>
#include <xrpl/tx/paths/RippleCalc.h>

#include <optional>

namespace edgy {

#ifdef EDGY_XAHAU
inline xrpl::path::RippleCalc::Output
rippleCalculate(
    xrpl::PaymentSandbox& view,
    xrpl::STAmount const& saMax,
    xrpl::STAmount const& saDst,
    xrpl::AccountID const& dst,
    xrpl::AccountID const& src,
    xrpl::STPathSet const& paths,
    std::optional<xrpl::uint256> const&,
    PathServices& services,
    xrpl::path::RippleCalc::Input const* input)
{
    return xrpl::path::RippleCalc::rippleCalculate(
        view, saMax, saDst, dst, src, paths, services.getLogs(), input);
}
#else
inline xrpl::path::RippleCalc::Output
rippleCalculate(
    xrpl::PaymentSandbox& view,
    xrpl::STAmount const& saMax,
    xrpl::STAmount const& saDst,
    xrpl::AccountID const& dst,
    xrpl::AccountID const& src,
    xrpl::STPathSet const& paths,
    std::optional<xrpl::uint256> const& domain,
    PathServices& services,
    xrpl::path::RippleCalc::Input const* input)
{
    // Payment::doApply installs this guard. AMM swap rounding and
    // changeSpotPriceQuality read getCurrentTransactionRules(), not
    // view.rules(). Without it, no-fee AMM spot beats the XRP/XAH CLOB
    // and maxOffer swallows the whole send_max (~8710 instead of ~8760).
    xrpl::CurrentTransactionRulesGuard const rules{view.rules()};
    return xrpl::path::RippleCalc::rippleCalculate(
        view, saMax, saDst, dst, src, paths, domain, services, input);
}
#endif

#ifdef EDGY_XAHAU
template <class T, class... Fs>
auto
visitAsset(T const& asset, Fs&&... fs)
{
    struct Overload : Fs...
    {
        using Fs::operator()...;
    };
    if constexpr (requires { asset.value(); })
        return std::visit(Overload{std::forward<Fs>(fs)...}, asset.value());
    else
        return asset.visit(std::forward<Fs>(fs)...);
}
#else
template <class T, class... Fs>
auto
visitAsset(T const& asset, Fs&&... fs)
{
    return asset.visit(std::forward<Fs>(fs)...);
}
#endif

}  // namespace edgy
