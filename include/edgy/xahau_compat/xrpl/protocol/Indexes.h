#pragma once

#include_next <xrpl/protocol/Indexes.h>

namespace ripple::keylet {

inline Keylet const&
feeSettings() noexcept
{
    return fees();
}

inline Keylet
mptokenIssuance(MPTID const& issuanceID) noexcept
{
    return mptIssuance(issuanceID);
}

}  // namespace ripple::keylet
