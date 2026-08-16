#include <edgy/memory_ledger.hpp>

#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/digest.h>

#include <algorithm>
#include <utility>

namespace edgy {
namespace {

bool
keepDecoded(xrpl::SLE const& sle)
{
    switch (sle.getType())
    {
        case xrpl::ltOFFER:
        case xrpl::ltAMM:
        case xrpl::ltFEE_SETTINGS:
        case xrpl::ltAMENDMENTS:
            return true;
        case xrpl::ltDIR_NODE:
            return sle.isFieldPresent(xrpl::sfExchangeRate);
        default:
            return false;
    }
}

void
applyFeesFromSle(xrpl::Fees& fees, xrpl::SLE::const_ref sle)
{
    if (sle->isFieldPresent(xrpl::sfBaseFee))
        fees.base = xrpl::XRPAmount{static_cast<std::int64_t>(sle->getFieldU64(xrpl::sfBaseFee))};
    if (sle->isFieldPresent(xrpl::sfReserveBase))
        fees.reserve = xrpl::XRPAmount{sle->getFieldU32(xrpl::sfReserveBase)};
    if (sle->isFieldPresent(xrpl::sfReserveIncrement))
        fees.increment = xrpl::XRPAmount{sle->getFieldU32(xrpl::sfReserveIncrement)};

    auto assignDrops = [&](xrpl::XRPAmount& dest, xrpl::SField const& field) {
        if (!sle->isFieldPresent(field))
            return;
        auto const amt = sle->getFieldAmount(field);
        if (amt.native())
            dest = amt.xrp();
    };
    assignDrops(fees.base, xrpl::sfBaseFeeDrops);
    assignDrops(fees.reserve, xrpl::sfReserveBaseDrops);
    assignDrops(fees.increment, xrpl::sfReserveIncrementDrops);
}

xrpl::Blob
serializeSle(xrpl::SLE::const_ref sle)
{
    xrpl::Serializer s;
    sle->add(s);
    return s.getData();
}

}  // namespace

class MemoryLedger::SlesIter : public SlesType::iter_base
{
public:
    SlesIter(MemoryLedger const* view, std::optional<key_type> current, bool end)
        : view_(view), current_(std::move(current)), end_(end)
    {
    }

    [[nodiscard]] std::unique_ptr<base_type>
    copy() const override
    {
        return std::make_unique<SlesIter>(*this);
    }

    [[nodiscard]] bool
    equal(base_type const& impl) const override
    {
        auto const p = dynamic_cast<SlesIter const*>(&impl);
        if (!p)
            return false;
        if (end_ || p->end_)
            return end_ && p->end_;
        return current_ == p->current_;
    }

    void
    increment() override
    {
        if (end_ || !current_)
        {
            end_ = true;
            current_.reset();
            return;
        }
        current_ = view_->succ(*current_);
        if (!current_)
            end_ = true;
    }

    [[nodiscard]] SlesType::value_type
    dereference() const override
    {
        if (end_ || !current_)
            return nullptr;
        return view_->sleOf(*current_);
    }

private:
    MemoryLedger const* view_;
    std::optional<key_type> current_;
    bool end_{false};
};

class MemoryLedger::TxsIter : public TxsType::iter_base
{
public:
    [[nodiscard]] std::unique_ptr<base_type>
    copy() const override
    {
        return std::make_unique<TxsIter>(*this);
    }

    [[nodiscard]] bool
    equal(base_type const& impl) const override
    {
        return dynamic_cast<TxsIter const*>(&impl) != nullptr;
    }

    void
    increment() override
    {
    }

    [[nodiscard]] TxsType::value_type
    dereference() const override
    {
        return {nullptr, nullptr};
    }
};

MemoryLedger::MemoryLedger(
    xrpl::LedgerHeader header,
    xrpl::Fees fees,
    xrpl::Rules rules,
    std::shared_ptr<Base const> base,
    std::shared_ptr<Overlay const> overlay,
    bool open)
    : header_(header)
    , fees_(fees)
    , rules_(std::move(rules))
    , base_(std::move(base))
    , overlay_(std::move(overlay))
    , open_(open)
{
    if (!base_)
        base_ = std::make_shared<Base>();
    if (!overlay_)
        overlay_ = std::make_shared<Overlay>();
}

xrpl::LedgerHeader const&
MemoryLedger::header() const
{
    return header_;
}

bool
MemoryLedger::open() const
{
    return open_;
}

xrpl::Fees const&
MemoryLedger::fees() const
{
    return fees_;
}

xrpl::Rules const&
MemoryLedger::rules() const
{
    return rules_;
}

MemoryLedger::Item const*
MemoryLedger::findItem(xrpl::uint256 const& key) const
{
    if (auto const it = overlay_->find(key); it != overlay_->end())
        return it->second ? &*it->second : nullptr;
    if (auto const it = base_->items.find(key); it != base_->items.end())
        return &it->second;
    return nullptr;
}

bool
MemoryLedger::live(xrpl::uint256 const& key) const
{
    return findItem(key) != nullptr;
}

std::optional<MemoryLedger::key_type>
MemoryLedger::firstKey() const
{
    xrpl::uint256 zero{};
    if (live(zero))
        return zero;
    return succ(zero);
}

xrpl::SLE::const_pointer
MemoryLedger::materialize(xrpl::uint256 const& key, Item const& item) const
{
    std::mutex& mutex =
        overlay_->contains(key) ? decodeMutex_ : base_->decodeMutex;
    std::lock_guard const lock(mutex);
    if (item.sle)
        return item.sle;

    try
    {
        xrpl::SerialIter sit(xrpl::makeSlice(item.blob));
        item.sle = std::make_shared<xrpl::SLE const>(sit, key);
        return item.sle;
    }
    catch (...)
    {
        return nullptr;
    }
}

xrpl::SLE::const_pointer
MemoryLedger::sleOf(xrpl::uint256 const& key) const
{
    auto const* item = findItem(key);
    if (!item)
        return nullptr;
    return materialize(key, *item);
}

bool
MemoryLedger::exists(xrpl::Keylet const& k) const
{
    auto const sle = sleOf(k.key);
    return sle && k.check(*sle);
}

std::optional<MemoryLedger::key_type>
MemoryLedger::succ(key_type const& key, std::optional<key_type> const& last) const
{
    auto bit = std::upper_bound(base_->keys.begin(), base_->keys.end(), key);
    auto oit = overlay_->upper_bound(key);

    while (true)
    {
        bool const haveB = bit != base_->keys.end();
        bool const haveO = oit != overlay_->end();
        if (!haveB && !haveO)
            return std::nullopt;

        key_type cand;
        bool fromOverlay = false;
        if (!haveB)
        {
            cand = oit->first;
            fromOverlay = true;
        }
        else if (!haveO)
        {
            cand = *bit;
        }
        else if (oit->first <= *bit)
        {
            cand = oit->first;
            fromOverlay = true;
        }
        else
        {
            cand = *bit;
        }

        if (last && cand >= *last)
            return std::nullopt;

        if (fromOverlay)
        {
            bool const present = oit->second.has_value();
            if (haveB && *bit == cand)
                ++bit;
            ++oit;
            if (present)
                return cand;
            continue;
        }

        return cand;
    }
}

xrpl::SLE::const_pointer
MemoryLedger::read(xrpl::Keylet const& k) const
{
    auto const sle = sleOf(k.key);
    if (!sle || !k.check(*sle))
        return nullptr;
    return sle;
}

std::unique_ptr<MemoryLedger::SlesType::iter_base>
MemoryLedger::slesBegin() const
{
    auto const first = firstKey();
    return std::make_unique<SlesIter>(this, first, !first);
}

std::unique_ptr<MemoryLedger::SlesType::iter_base>
MemoryLedger::slesEnd() const
{
    return std::make_unique<SlesIter>(this, std::nullopt, true);
}

std::unique_ptr<MemoryLedger::SlesType::iter_base>
MemoryLedger::slesUpperBound(key_type const& key) const
{
    auto const next = succ(key);
    return std::make_unique<SlesIter>(this, next, !next);
}

std::unique_ptr<MemoryLedger::TxsType::iter_base>
MemoryLedger::txsBegin() const
{
    return std::make_unique<TxsIter>();
}

std::unique_ptr<MemoryLedger::TxsType::iter_base>
MemoryLedger::txsEnd() const
{
    return std::make_unique<TxsIter>();
}

bool
MemoryLedger::txExists(key_type const&) const
{
    return false;
}

MemoryLedger::tx_type
MemoryLedger::txRead(key_type const&) const
{
    return {nullptr, nullptr};
}

std::optional<MemoryLedger::digest_type>
MemoryLedger::digest(key_type const& key) const
{
    auto const* item = findItem(key);
    if (!item)
        return std::nullopt;
    return xrpl::sha512Half(xrpl::makeSlice(item->blob));
}

std::size_t
MemoryLedger::size() const
{
    std::size_t n = base_->items.size();
    for (auto const& [k, v] : *overlay_)
    {
        bool const inBase = base_->items.contains(k);
        if (v)
        {
            if (!inBase)
                ++n;
        }
        else if (inBase)
        {
            --n;
        }
    }
    return n;
}

std::size_t
MemoryLedger::overlaySize() const
{
    return overlay_->size();
}

LedgerBuilder::LedgerBuilder()
    : base_(std::make_shared<MemoryLedger::Base>())
    , overlay_(std::make_shared<MemoryLedger::Overlay>())
{
}

void
LedgerBuilder::setHeader(xrpl::LedgerHeader header)
{
    header_ = header;
    header_.validated = true;
    header_.accepted = true;
}

void
LedgerBuilder::setFees(xrpl::Fees fees)
{
    fees_ = fees;
}

void
LedgerBuilder::setRules(xrpl::Rules rules)
{
    rules_ = std::move(rules);
}

void
LedgerBuilder::reserve(std::size_t n)
{
    if (!frozen_ && base_)
        base_->items.reserve(n);
}

void
LedgerBuilder::cowOverlay()
{
    if (!overlay_)
        overlay_ = std::make_shared<MemoryLedger::Overlay>();
    else if (overlay_.use_count() > 1)
        overlay_ = std::make_shared<MemoryLedger::Overlay>(*overlay_);
}

void
LedgerBuilder::freezeBase()
{
    if (frozen_ || !base_)
        return;
    base_->keys.clear();
    base_->keys.reserve(base_->items.size());
    for (auto const& [k, _] : base_->items)
        base_->keys.push_back(k);
    std::ranges::sort(base_->keys);
    frozen_ = true;
}

void
LedgerBuilder::put(xrpl::uint256 const& key, MemoryLedger::Item item)
{
    if (!frozen_)
    {
        if (!base_)
            base_ = std::make_shared<MemoryLedger::Base>();
        base_->items[key] = std::move(item);
        return;
    }
    cowOverlay();
    (*overlay_)[key] = std::move(item);
}

void
LedgerBuilder::upsert(xrpl::SLE::const_pointer sle)
{
    if (!sle)
        return;
    MemoryLedger::Item item;
    item.blob = serializeSle(sle);
    item.sle = std::move(sle);
    put(item.sle->key(), std::move(item));
}

void
LedgerBuilder::upsertRaw(
    xrpl::uint256 const& key,
    xrpl::Blob blob,
    xrpl::SLE::const_pointer decoded)
{
    MemoryLedger::Item item;
    item.blob = std::move(blob);
    if (decoded && keepDecoded(*decoded))
        item.sle = std::move(decoded);
    put(key, std::move(item));
}

void
LedgerBuilder::erase(xrpl::uint256 const& key)
{
    if (!frozen_)
    {
        if (base_)
            base_->items.erase(key);
        return;
    }
    cowOverlay();
    (*overlay_)[key] = std::nullopt;
}

void
LedgerBuilder::clear()
{
    base_ = std::make_shared<MemoryLedger::Base>();
    overlay_ = std::make_shared<MemoryLedger::Overlay>();
    frozen_ = false;
    open_ = false;
}

bool
LedgerBuilder::contains(xrpl::uint256 const& key) const
{
    if (overlay_)
    {
        if (auto const it = overlay_->find(key); it != overlay_->end())
            return it->second.has_value();
    }
    return base_ && base_->items.contains(key);
}

std::size_t
LedgerBuilder::size() const
{
    std::size_t n = base_ ? base_->items.size() : 0;
    if (!overlay_)
        return n;
    for (auto const& [k, v] : *overlay_)
    {
        bool const inBase = base_ && base_->items.contains(k);
        if (v)
        {
            if (!inBase)
                ++n;
        }
        else if (inBase)
        {
            --n;
        }
    }
    return n;
}

void
LedgerBuilder::applyFeeAndRules(MemoryLedger const& view)
{
    if (auto const sle = view.read(xrpl::keylet::feeSettings()))
        applyFeesFromSle(fees_, sle);
    rules_ = xrpl::makeRulesGivenLedger(view, rules_);
}

std::shared_ptr<MemoryLedger const>
LedgerBuilder::publish()
{
    if (!base_)
        base_ = std::make_shared<MemoryLedger::Base>();
    if (!overlay_)
        overlay_ = std::make_shared<MemoryLedger::Overlay>();
    freezeBase();
    std::shared_ptr<MemoryLedger::Base const> frozenBase = base_;
    std::shared_ptr<MemoryLedger::Overlay const> frozenOverlay = overlay_;
    auto view = std::make_shared<MemoryLedger>(
        header_, fees_, rules_, frozenBase, frozenOverlay, open_);
    applyFeeAndRules(*view);
    view = std::make_shared<MemoryLedger>(
        header_, fees_, rules_, frozenBase, frozenOverlay, open_);
    return view;
}

}  // namespace edgy
