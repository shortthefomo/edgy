#pragma once

#include <edgy/ripple_calc.hpp>

#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/MPTIssue.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/SystemParameters.h>

namespace xrpl {

inline STAmount
largestAmount(STAmount const& amt)
{
#ifdef EDGY_XAHAU
    auto const nativeXrp = INITIAL_XRP;
    auto const maxMpt = std::uint64_t{0xFFFF'FFFF'FFFF'FFFFULL};
    auto const maxValue = STAmount::cMaxValue;
    auto const maxOffset = STAmount::cMaxOffset;
#else
    auto const nativeXrp = kInitialXrp;
    auto const maxMpt = kMaxMpTokenAmount;
    auto const maxValue = STAmount::kMaxValue;
    auto const maxOffset = STAmount::kMaxOffset;
#endif
    return edgy::visitAsset(
        amt.asset(),
        [&](Issue const& issue) -> STAmount {
            if (issue.native())
                return nativeXrp;
            return STAmount(amt.asset(), maxValue, maxOffset);
        },
        [&](MPTIssue const&) { return STAmount(amt.asset(), maxMpt, 0); });
}

inline STAmount
convertAmount(STAmount const& amt, bool all)
{
    if (!all)
        return amt;

    return largestAmount(amt);
};

inline bool
convertAllCheck(STAmount const& a)
{
    return a == largestAmount(a);
}

}  // namespace xrpl
