#pragma once

#include <xrpl/protocol/Asset.h>
#include <xrpl/protocol/Book.h>
#include <xrpl/protocol/Issue.h>

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

}  // namespace edgy
