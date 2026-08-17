#pragma once

#include <xrpl/protocol/Asset.h>
#include <xrpl/protocol/Book.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/Quality.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STPathSet.h>

#include <cmath>
#include <optional>

namespace edgy {

#ifdef EDGY_XAHAU
inline bool
bookHasDomain(xrpl::Book const&)
{
    return false;
}

inline xrpl::Book
makeBook(
    xrpl::Asset const& in,
    xrpl::Asset const& out,
    std::optional<xrpl::uint256> const& = std::nullopt)
{
    auto asIssue = [](xrpl::Asset const& a) -> xrpl::Issue {
        if (a.holds<xrpl::Issue>())
            return a.get<xrpl::Issue>();
        return xrpl::xrpIssue();
    };
    return {asIssue(in), asIssue(out)};
}
#else
inline bool
bookHasDomain(xrpl::Book const& book)
{
    return book.domain.has_value();
}

inline xrpl::Book
makeBook(
    xrpl::Asset const& in,
    xrpl::Asset const& out,
    std::optional<xrpl::uint256> const& domain = std::nullopt)
{
    return {in, out, domain};
}
#endif

[[nodiscard]] inline xrpl::Quality
composeQuality(xrpl::Quality const& lhs, xrpl::Quality const& rhs)
{
#ifdef EDGY_XAHAU
    return xrpl::composed_quality(lhs, rhs);
#else
    return xrpl::composedQuality(lhs, rhs);
#endif
}

[[nodiscard]] inline xrpl::Keylet
trustLineKeylet(xrpl::AccountID const& account, xrpl::Issue const& issue)
{
#ifdef EDGY_XAHAU
    return xrpl::keylet::line(account, issue);
#else
    return xrpl::keylet::trustLine(account, issue);
#endif
}

// Decimal value of an STAmount (drops for native, IOU as mantissa*10^exp).
[[nodiscard]] inline double
amountAsDouble(xrpl::STAmount const& amt)
{
    if (amt.negative() || amt.signum() == 0)
        return 0;
    return static_cast<double>(amt.mantissa()) * std::pow(10.0, amt.exponent());
}

// Packed ExchangeRate / getRate as in/out. 0 is an extremely good offer.
[[nodiscard]] inline double
qualityRatio(xrpl::Quality const& q)
{
    return amountAsDouble(q.rate());
}

[[nodiscard]] inline xrpl::Asset
pathElementAsset(xrpl::STPathElement const& el)
{
#ifdef EDGY_XAHAU
    if (xrpl::isXRP(el.getCurrency()))
        return xrpl::xrpIssue();
    return xrpl::Issue{el.getCurrency(), el.getIssuerID()};
#else
    return el.getPathAsset().visit(
        [&](xrpl::Currency const& c) -> xrpl::Asset {
            return xrpl::Issue{c, el.getIssuerID()};
        },
        [](xrpl::MPTID const& m) -> xrpl::Asset { return xrpl::MPTIssue{m}; });
#endif
}

}  // namespace edgy
