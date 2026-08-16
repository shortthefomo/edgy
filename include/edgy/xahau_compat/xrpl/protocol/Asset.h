#pragma once

#include_next <xrpl/protocol/Asset.h>

namespace ripple {

inline bool
validAsset(Asset const& asset)
{
    if (asset.holds<Issue>())
    {
        auto const& issue = asset.get<Issue>();
        return isConsistent(issue) && issue.currency != badCurrency();
    }
    return asset.getIssuer() != xrpAccount();
}

}  // namespace ripple
