#pragma once

#include <edgy/compat.hpp>

#include <xrpl/protocol/Asset.h>
#include <xrpl/protocol/Book.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/Quality.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STPathSet.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/SField.h>
#ifdef EDGY_XAHAU
#include <xrpld/ledger/View.h>
#else
#include <xrpl/ledger/helpers/TokenHelpers.h>
#endif

#include <cmath>
#include <cstdint>
#include <optional>

namespace edgy {

#ifdef EDGY_XAHAU
inline bool
bookHasDomain(xrpl::Book const&)
{
    return false;
}

inline xrpl::Book
makeBook(
    xrpl::Asset const& in,
    xrpl::Asset const& out,
    std::optional<xrpl::uint256> const& = std::nullopt)
{
    auto asIssue = [](xrpl::Asset const& a) -> xrpl::Issue {
        if (a.holds<xrpl::Issue>())
            return a.get<xrpl::Issue>();
        return xrpl::xrpIssue();
    };
    return {asIssue(in), asIssue(out)};
}
#else
inline bool
bookHasDomain(xrpl::Book const& book)
{
    return book.domain.has_value();
}

inline xrpl::Book
makeBook(
    xrpl::Asset const& in,
    xrpl::Asset const& out,
    std::optional<xrpl::uint256> const& domain = std::nullopt)
{
    return {in, out, domain};
}
#endif

[[nodiscard]] inline xrpl::Quality
composeQuality(xrpl::Quality const& lhs, xrpl::Quality const& rhs)
{
#ifdef EDGY_XAHAU
    return xrpl::composed_quality(lhs, rhs);
#else
    return xrpl::composedQuality(lhs, rhs);
#endif
}

[[nodiscard]] inline xrpl::Keylet
trustLineKeylet(xrpl::AccountID const& account, xrpl::Issue const& issue)
{
#ifdef EDGY_XAHAU
    return xrpl::keylet::line(account, issue);
#else
    return xrpl::keylet::trustLine(account, issue);
#endif
}

// Decimal value of an STAmount (drops for native, IOU as mantissa*10^exp).
[[nodiscard]] inline double
amountAsDouble(xrpl::STAmount const& amt)
{
    if (amt.negative() || amt.signum() == 0)
        return 0;
    return static_cast<double>(amt.mantissa()) * std::pow(10.0, amt.exponent());
}

// Packed ExchangeRate / getRate as in/out. 0 is an extremely good offer.
[[nodiscard]] inline double
qualityRatio(xrpl::Quality const& q)
{
    return amountAsDouble(q.rate());
}

[[nodiscard]] inline xrpl::Asset
pathElementAsset(xrpl::STPathElement const& el)
{
#ifdef EDGY_XAHAU
    if (xrpl::isXRP(el.getCurrency()))
        return xrpl::xrpIssue();
    return xrpl::Issue{el.getCurrency(), el.getIssuerID()};
#else
    return el.getPathAsset().visit(
        [&](xrpl::Currency const& c) -> xrpl::Asset {
            return xrpl::Issue{c, el.getIssuerID()};
        },
        [](xrpl::MPTID const& m) -> xrpl::Asset { return xrpl::MPTIssue{m}; });
#endif
}

#ifdef EDGY_XAHAU
inline xrpl::STAmount
offerOwnerFunds(
    xrpl::ReadView const& view,
    xrpl::AccountID const& owner,
    xrpl::STAmount const& gets,
    beast::Journal j)
{
    return xrpl::accountFunds(view, owner, gets, xrpl::fhZERO_IF_FROZEN, j);
}

inline xrpl::Amounts
limitOfferOut(
    xrpl::Quality const& q,
    xrpl::Amounts const& ofr,
    xrpl::STAmount const& funds)
{
    return q.ceil_out_strict(ofr, funds, false);
}

inline xrpl::Amounts
limitOfferIn(
    xrpl::Quality const& q,
    xrpl::Amounts const& ofr,
    xrpl::STAmount const& left)
{
    return q.ceil_in_strict(ofr, left, false);
}
#else
inline xrpl::STAmount
offerOwnerFunds(
    xrpl::ReadView const& view,
    xrpl::AccountID const& owner,
    xrpl::STAmount const& gets,
    beast::Journal j)
{
    return xrpl::accountFunds(
        view, owner, gets, xrpl::FreezeHandling::ZeroIfFrozen, j);
}

inline xrpl::Amounts
limitOfferOut(
    xrpl::Quality const& q,
    xrpl::Amounts const& ofr,
    xrpl::STAmount const& funds)
{
    return q.ceilOutStrict(ofr, funds, false);
}

inline xrpl::Amounts
limitOfferIn(
    xrpl::Quality const& q,
    xrpl::Amounts const& ofr,
    xrpl::STAmount const& left)
{
    return q.ceilInStrict(ofr, left, false);
}
#endif

// Why a CLOB walk produced no output. EmptyBook with dirs/offers > 0 or
// Threw is an invariant miss — the previous OfferStream walker swallowed
// leftover-dust exceptions as a silent nullopt.
enum class ClobWalkWhy : std::uint8_t
{
    Ok = 0,
    EmptyWant,
    NotApplicable,
    EmptyBook,
    Threw,
};

[[nodiscard]] inline char const*
clobWalkWhyText(ClobWalkWhy why)
{
    switch (why)
    {
        case ClobWalkWhy::Ok:
            return "ok";
        case ClobWalkWhy::EmptyWant:
            return "empty_want";
        case ClobWalkWhy::NotApplicable:
            return "not_applicable";
        case ClobWalkWhy::EmptyBook:
            return "empty_book";
        case ClobWalkWhy::Threw:
            return "threw";
    }
    return "unknown";
}

struct ClobWalkResult
{
    std::optional<xrpl::STAmount> out;
    ClobWalkWhy why{ClobWalkWhy::EmptyBook};
    int dirs{0};
    int offers{0};
    int taken{0};
    int skippedUnfunded{0};
    int skippedOther{0};

    [[nodiscard]] bool
    ok() const noexcept
    {
        return why == ClobWalkWhy::Ok && out && *out > beast::kZero;
    }
};

// Silent nullopt that hid a live book (dirs or offers present, or a throw).
[[nodiscard]] inline bool
clobWalkIsFault(ClobWalkResult const& r) noexcept
{
    return r.why == ClobWalkWhy::Threw ||
        (r.why == ClobWalkWhy::EmptyBook && (r.dirs > 0 || r.offers > 0));
}

// Spend `want` down a CLOB (no AMM).
[[nodiscard]] inline ClobWalkResult
clobBookTake(
    xrpl::ReadView const& view,
    xrpl::Book const& book,
    xrpl::STAmount const& want)
{
    ClobWalkResult r;
    if (want <= beast::kZero)
    {
        r.why = ClobWalkWhy::EmptyWant;
        return r;
    }
    try
    {
        beast::Journal const j{beast::Journal::getNullSink()};
        auto const expire = viewHeader(view).parentCloseTime;
        xrpl::STAmount left = want;
        xrpl::STAmount got{book.out, 0};
        auto const start = xrpl::getBookBase(book);
        auto const end = xrpl::getQualityNext(start);
        int seen = 0;
        for (auto k = view.succ(start, end); k && seen < 256 && left > beast::kZero;
             k = view.succ(*k, end))
        {
            ++r.dirs;
            auto const dir = view.read(xrpl::keylet::unchecked(*k));
            if (!dir || !dir->isFieldPresent(xrpl::sfIndexes))
            {
                ++r.skippedOther;
                continue;
            }
            xrpl::Quality const q{xrpl::getQuality(*k)};
            for (auto const& idx : dir->getFieldV256(xrpl::sfIndexes))
            {
                if (seen++ >= 256 || left <= beast::kZero)
                    break;
                ++r.offers;
                auto const off = view.read(xrpl::keylet::offer(idx));
                if (!off || !off->isFieldPresent(xrpl::sfTakerPays) ||
                    !off->isFieldPresent(xrpl::sfTakerGets))
                {
                    ++r.skippedOther;
                    continue;
                }
                if (off->isFieldPresent(xrpl::sfExpiration))
                {
                    using Duration = xrpl::NetClock::duration;
                    using TimePoint = xrpl::NetClock::time_point;
                    if (TimePoint{Duration{off->getFieldU32(xrpl::sfExpiration)}} <= expire)
                    {
                        ++r.skippedOther;
                        continue;
                    }
                }
                auto pays = off->getFieldAmount(xrpl::sfTakerPays);
                auto gets = off->getFieldAmount(xrpl::sfTakerGets);
#ifdef EDGY_XAHAU
                if (!pays.holds<xrpl::Issue>() || !gets.holds<xrpl::Issue>() ||
                    pays.get<xrpl::Issue>() != book.in ||
                    gets.get<xrpl::Issue>() != book.out ||
#else
                if (pays.asset() != book.in || gets.asset() != book.out ||
#endif
                    pays <= beast::kZero || gets <= beast::kZero)
                {
                    ++r.skippedOther;
                    continue;
                }
                auto const funds = offerOwnerFunds(
                    view, off->getAccountID(xrpl::sfAccount), gets, j);
                if (funds <= beast::kZero)
                {
                    ++r.skippedUnfunded;
                    continue;
                }
                xrpl::Amounts ofr{pays, gets};
                if (funds < ofr.out)
                    ofr = limitOfferOut(q, ofr, funds);
                if (ofr.empty())
                {
                    ++r.skippedOther;
                    continue;
                }
                if (ofr.in > left)
                    ofr = limitOfferIn(q, ofr, left);
                if (ofr.empty() || ofr.in > left || ofr.in <= beast::kZero)
                {
                    ++r.skippedOther;
                    continue;
                }
                got += ofr.out;
                ++r.taken;
                if (ofr.in >= left)
                    left = xrpl::STAmount{left.asset(), 0};
                else
                    left -= ofr.in;
            }
        }
        if (got <= beast::kZero)
        {
            r.why = ClobWalkWhy::EmptyBook;
            return r;
        }
        r.out = got;
        r.why = ClobWalkWhy::Ok;
        return r;
    }
    catch (...)
    {
        r.why = ClobWalkWhy::Threw;
        r.out.reset();
        return r;
    }
}

[[nodiscard]] inline ClobWalkResult
nativeBridgeClobOut(
    xrpl::ReadView const& view,
    xrpl::STAmount const& saMax,
    xrpl::Asset const& destAsset)
{
    ClobWalkResult r;
    if (!saMax.holds<xrpl::Issue>() || xrpl::isXRP(saMax) || xrpl::isXRP(destAsset))
    {
        r.why = ClobWalkWhy::NotApplicable;
        return r;
    }
    auto const native = xrpl::xrpIssue();
    auto first = clobBookTake(view, makeBook(saMax.asset(), native), saMax);
    if (!first.ok())
        return first;
    auto second = clobBookTake(view, makeBook(native, destAsset), *first.out);
    second.dirs += first.dirs;
    second.offers += first.offers;
    second.taken += first.taken;
    second.skippedUnfunded += first.skippedUnfunded;
    second.skippedOther += first.skippedOther;
    return second;
}

}  // namespace edgy
