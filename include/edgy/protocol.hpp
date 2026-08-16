#pragma once

#include <edgy/config.hpp>

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/STLedgerEntry.h>

#include <string>

namespace edgy {

// Parse a ledger object, dropping field codes libxrpl does not know
// (xahaud Hook/reward extras). Returns nullptr if the type itself is
// unknown or required path-find fields cannot be recovered.
[[nodiscard]] xrpl::SLE::pointer
sleFromBlob(xrpl::Blob const& blob, xrpl::uint256 const& key);

[[nodiscard]] xrpl::SLE::pointer
sleFromBinary(std::string const& dataHex, std::string const& indexHex);

// Drop JSON keys that are not SFields so STParsedJSON can apply
// AccountRoot / Offer / RippleState updates from a xahaud stream.
// Does not walk into amount/issue objects (currency/issuer/value).
void
stripUnknownJsonFields(json::Value& v);

// Map native ticker XAH <-> XRP around libxrpl (which only speaks XRP).
void
rewriteNativeJsonIn(json::Value& v, NetworkKind network);

void
rewriteNativeJsonOut(json::Value& v, NetworkKind network);

}  // namespace edgy
