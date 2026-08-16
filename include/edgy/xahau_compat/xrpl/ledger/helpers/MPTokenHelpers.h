#pragma once

#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/UintTypes.h>

namespace ripple {

// rippled helper used by AssetCache::getMPTs. xahaud has the ledger
// types but not this header; outstanding == max is "maxed out".
inline std::uint64_t
maxMPTAmount(STLedgerEntry const& sle)
{
    if (sle.isFieldPresent(sfMaximumAmount))
        return sle.getFieldU64(sfMaximumAmount);
    return 0;
}

}  // namespace ripple
