#pragma once

#include_next <xrpl/protocol/UintTypes.h>

namespace ripple {

inline bool
toCurrency(Currency& c, std::string const& s)
{
    return to_currency(c, s);
}

}  // namespace ripple
