#pragma once

#include_next <xrpl/protocol/AccountID.h>

namespace ripple {

inline bool
toIssuer(AccountID& id, std::string const& s)
{
    return to_issuer(id, s);
}

}  // namespace ripple
