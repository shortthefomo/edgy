#pragma once

#include <edgy/compat.hpp>

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/Fees.h>
#include <xrpl/protocol/Keylet.h>
#include <xrpl/protocol/LedgerHeader.h>
#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/STLedgerEntry.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace edgy {

// Rules::Impl stores a reference to this set. It must outlive every Rules
// object. A temporary (Rules{{}}) dangles and SIGSEGVs in enabled() once
// the heap is reused — typically inside RippleCalc / checkFreeze.
[[nodiscard]] inline std::unordered_set<xrpl::uint256, beast::Uhash<>> const&
emptyRulePresets()
{
    static std::unordered_set<xrpl::uint256, beast::Uhash<>> const kEmpty;
    return kEmpty;
}

/**
 * Immutable in-memory ledger tuned for path_find.
 *
 * Snapshot objects live in an immutable hash map (O(1) read) plus a sorted
 * key vector (O(log n) succ for BookTip). Ledger closes write a small overlay
 * instead of cloning ~19M objects. Decoded SLEs stick to the item they came
 * from so BookTip / RippleCalc never re-parse the same offer.
 */
class MemoryLedger final : public xrpl::DigestAwareReadView
{
public:
    struct Item
    {
        xrpl::Blob blob;
        mutable std::shared_ptr<xrpl::SLE const> sle;
    };

    struct Base
    {
        xrpl::hardened_hash_map<xrpl::uint256, Item> items;
        std::vector<xrpl::uint256> keys;
        mutable std::mutex decodeMutex;
    };

    // nullopt value = deleted since the snapshot.
    using Overlay = std::map<xrpl::uint256, std::optional<Item>>;

    MemoryLedger(
        xrpl::LedgerHeader header,
        xrpl::Fees fees,
        xrpl::Rules rules,
        std::shared_ptr<Base const> base,
        std::shared_ptr<Overlay const> overlay,
        bool open = false);

#ifdef EDGY_XAHAU
    using SlesType = sles_type;
    using TxsType = txs_type;

    [[nodiscard]] xrpl::LedgerInfo const&
    info() const override;

    [[nodiscard]] xrpl::LedgerHeader const&
    header() const
    {
        return header_;
    }
#else
    [[nodiscard]] xrpl::LedgerHeader const&
    header() const override;
#endif

    [[nodiscard]] bool
    open() const override;

    [[nodiscard]] xrpl::Fees const&
    fees() const override;

    [[nodiscard]] xrpl::Rules const&
    rules() const override;

    [[nodiscard]] bool
    exists(xrpl::Keylet const& k) const override;

    [[nodiscard]] std::optional<key_type>
    succ(key_type const& key, std::optional<key_type> const& last = std::nullopt) const override;

    [[nodiscard]] std::shared_ptr<xrpl::SLE const>
    read(xrpl::Keylet const& k) const override;

    [[nodiscard]] std::unique_ptr<SlesType::iter_base>
    slesBegin() const override;

    [[nodiscard]] std::unique_ptr<SlesType::iter_base>
    slesEnd() const override;

    [[nodiscard]] std::unique_ptr<SlesType::iter_base>
    slesUpperBound(key_type const& key) const override;

    [[nodiscard]] std::unique_ptr<TxsType::iter_base>
    txsBegin() const override;

    [[nodiscard]] std::unique_ptr<TxsType::iter_base>
    txsEnd() const override;

    [[nodiscard]] bool
    txExists(key_type const& key) const override;

    [[nodiscard]] tx_type
    txRead(key_type const& key) const override;

    [[nodiscard]] std::optional<digest_type>
    digest(key_type const& key) const override;

    [[nodiscard]] std::size_t
    size() const;

    [[nodiscard]] std::size_t
    overlaySize() const;

    // Prefer overlay keys that already existed in the snapshot (modifies),
    // rotated by seed so we do not check the same four low keys every close.
    // Snapshot-only creates are skipped — they are often next-ledger objects
    // and show up as missing_node against ledger N.
    [[nodiscard]] std::vector<key_type>
    sampleKeys(std::size_t n, std::uint32_t seed = 0) const;

    [[nodiscard]] std::shared_ptr<xrpl::SLE const>
    sleOf(xrpl::uint256 const& key) const;

private:
    class SlesIter;
    class TxsIter;

    [[nodiscard]] Item const*
    findItem(xrpl::uint256 const& key) const;

    [[nodiscard]] bool
    live(xrpl::uint256 const& key) const;

    [[nodiscard]] std::optional<key_type>
    firstKey() const;

    [[nodiscard]] std::shared_ptr<xrpl::SLE const>
    materialize(xrpl::uint256 const& key, Item const& item) const;

    xrpl::LedgerHeader header_;
    xrpl::Fees fees_;
    xrpl::Rules rules_;
    std::shared_ptr<Base const> base_;
    std::shared_ptr<Overlay const> overlay_;
    bool open_{false};
    mutable std::mutex decodeMutex_;
};

/**
 * Copy-on-write builder. The snapshot hash map is frozen once and never
 * copied. Subsequent upsert/erase clone only the overlay.
 */
class LedgerBuilder
{
public:
    LedgerBuilder();

    void
    setHeader(xrpl::LedgerHeader header);

    void
    setOpen(bool open)
    {
        open_ = open;
    }

    [[nodiscard]] bool
    open() const
    {
        return open_;
    }

    void
    setFees(xrpl::Fees fees);

    void
    setRules(xrpl::Rules rules);

    void
    reserve(std::size_t n);

    void
    upsert(std::shared_ptr<xrpl::SLE const> sle);

    void
    upsertRaw(
        xrpl::uint256 const& key,
        xrpl::Blob blob,
        std::shared_ptr<xrpl::SLE const> decoded = {});

    void
    erase(xrpl::uint256 const& key);

    void
    clear();

    [[nodiscard]] bool
    contains(xrpl::uint256 const& key) const;

    // Mutable clone of the current object, or nullptr if missing/deleted.
    [[nodiscard]] std::shared_ptr<xrpl::SLE>
    clone(xrpl::uint256 const& key) const;

    [[nodiscard]] xrpl::LedgerHeader const&
    header() const
    {
        return header_;
    }

    [[nodiscard]] std::size_t
    size() const;

    [[nodiscard]] std::shared_ptr<MemoryLedger const>
    publish();

private:
    void
    cowOverlay();

    void
    freezeBase();

    void
    put(xrpl::uint256 const& key, MemoryLedger::Item item);

    void
    applyFeeAndRules(MemoryLedger const& view);

    xrpl::LedgerHeader header_{};
    xrpl::Fees fees_{};
    xrpl::Rules rules_{emptyRulePresets()};
    std::shared_ptr<MemoryLedger::Base> base_;
    std::shared_ptr<MemoryLedger::Overlay> overlay_;
    bool frozen_{false};
    bool open_{false};
};

}  // namespace edgy
