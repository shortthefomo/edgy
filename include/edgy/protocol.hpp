#pragma once

#include <edgy/compat.hpp>
#include <edgy/config.hpp>

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/STLedgerEntry.h>

#include <memory>
#include <string>

namespace edgy {

// Parse a ledger object, dropping field codes libxrpl does not know
// (xahaud Hook/reward extras). Returns nullptr if the type itself is
// unknown or required path-find fields cannot be recovered.
[[nodiscard]] std::shared_ptr<xrpl::SLE>
sleFromBlob(xrpl::Blob const& blob, xrpl::uint256 const& key);

[[nodiscard]] std::shared_ptr<xrpl::SLE>
sleFromBinary(std::string const& dataHex, std::string const& indexHex);

// True if path-find state on `local` matches `node`. Bookkeeping fields
// (PreviousTxnID, directory node indexes) are ignored — apply rewrite
// changes those even when balances and books are correct. Returns the
// first differing field name, or nullptr when they match.
[[nodiscard]] char const*
sleFirstMismatch(xrpl::STObject const& local, xrpl::STObject const& node);

[[nodiscard]] inline bool
sleCoversNode(xrpl::STObject const& local, xrpl::STObject const& node)
{
    return sleFirstMismatch(local, node) == nullptr;
}

// "31!=32 +1-2" when Indexes sets differ; empty when they match.
[[nodiscard]] std::string
indexesDiffText(xrpl::STObject const& local, xrpl::STObject const& node);

// Drop JSON keys that are not SFields so STParsedJSON can apply
// AccountRoot / Offer / RippleState updates from a xahaud stream.
// Does not walk into amount/issue objects (currency/issuer/value).
void
stripUnknownJsonFields(json::Value& v);

// After strip: drop nested STObjects that are not amount/issue JSON so
// STParsedJSON can apply AccountRoot / Offer / RippleState from xahaud.
void
slimJsonMetaNode(json::Value& node);

// Map native ticker XAH <-> XRP around libxrpl (which only speaks XRP).
void
rewriteNativeJsonIn(json::Value& v, NetworkKind network);

void
rewriteNativeJsonOut(json::Value& v, NetworkKind network);

}  // namespace edgy
