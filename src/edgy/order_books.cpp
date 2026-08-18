#include <edgy/order_books.hpp>

#include <edgy/book_util.hpp>

#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/PathAsset.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/UintTypes.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

namespace edgy {
namespace {

bool
isBookDirRoot(xrpl::SLE const& sle)
{
    return sle.getType() == xrpl::ltDIR_NODE && sle.isFieldPresent(xrpl::sfExchangeRate) &&
        sle.isFieldPresent(xrpl::sfRootIndex) && sle.getFieldH256(xrpl::sfRootIndex) == sle.key();
}

std::optional<xrpl::Book>
bookFromDir(xrpl::SLE const& sle)
{
    xrpl::Book book;
    if (sle.isFieldPresent(xrpl::sfTakerPaysCurrency))
    {
        xrpl::Issue issue;
        issue.currency = sle.getFieldH160(xrpl::sfTakerPaysCurrency);
        issue.account = sle.getFieldH160(xrpl::sfTakerPaysIssuer);
        book.in = issue;
    }
#ifndef EDGY_XAHAU
    else if (sle.isFieldPresent(xrpl::sfTakerPaysMPT))
    {
        book.in = sle.getFieldH192(xrpl::sfTakerPaysMPT);
    }
#endif
    else
    {
        return std::nullopt;
    }

    if (sle.isFieldPresent(xrpl::sfTakerGetsCurrency))
    {
        xrpl::Issue issue;
        issue.currency = sle.getFieldH160(xrpl::sfTakerGetsCurrency);
        issue.account = sle.getFieldH160(xrpl::sfTakerGetsIssuer);
        book.out = issue;
    }
#ifndef EDGY_XAHAU
    else if (sle.isFieldPresent(xrpl::sfTakerGetsMPT))
    {
        book.out = sle.getFieldH192(xrpl::sfTakerGetsMPT);
    }
#endif
    else
    {
        return std::nullopt;
    }
#ifndef EDGY_XAHAU
    book.domain = sle[~xrpl::sfDomainID];
#endif
    return book;
}

std::optional<xrpl::Book>
bookFromOffer(xrpl::SLE const& sle)
{
    if (sle.getType() != xrpl::ltOFFER || !sle.isFieldPresent(xrpl::sfTakerPays) ||
        !sle.isFieldPresent(xrpl::sfTakerGets))
        return std::nullopt;
    auto const pays = sle.getFieldAmount(xrpl::sfTakerPays);
    auto const gets = sle.getFieldAmount(xrpl::sfTakerGets);
    if (pays.signum() <= 0 || gets.signum() <= 0)
        return std::nullopt;
#ifndef EDGY_XAHAU
    return makeBook(pays.asset(), gets.asset(), sle[~xrpl::sfDomainID]);
#else
    return makeBook(pays.asset(), gets.asset());
#endif
}

std::uint64_t
dirQuality(xrpl::SLE const& sle)
{
    std::uint64_t q = xrpl::getQuality(sle.key());
    if (sle.isFieldPresent(xrpl::sfExchangeRate))
        q = sle.getFieldU64(xrpl::sfExchangeRate);
    return q;
}

void
mergeBetter(BookEdge& dst, BookEdge const& src)
{
    if (src.amm)
        dst.amm = true;
    if (src.quality < dst.quality)
    {
        dst.quality = src.quality;
        dst.outSize = src.outSize;
    }
    else if (src.quality == dst.quality && src.outSize > dst.outSize)
    {
        dst.outSize = src.outSize;
    }
}

}  // namespace

void
LocalOrderBooks::addBookUnlocked(xrpl::Book const& book)
{
#ifndef EDGY_XAHAU
    if (book.domain)
    {
        domainBooks_[{book.in, *book.domain}].insert(book.out);
        reverseDomainBooks_[{book.out, *book.domain}].insert(book.in);
        if (xrpl::isXRP(book.out))
            xrpDomainBooks_.insert({book.in, *book.domain});
        return;
    }
#endif

    allBooks_[book.in].insert(book.out);
    reverseBooks_[book.out].insert(book.in);
    tokenFwd_[xrpl::PathAsset{book.in}].insert(book.out);
    tokenRev_[xrpl::PathAsset{book.out}].insert(book.in);
    if (xrpl::isXRP(book.out))
        xrpBooks_.insert(book.in);
}

void
LocalOrderBooks::noteQualityUnlocked(xrpl::Book const& book, std::uint64_t quality)
{
    setDirQualityUnlocked(book, quality);
}

void
LocalOrderBooks::setDirQualityUnlocked(xrpl::Book const& book, std::uint64_t quality)
{
    addBookUnlocked(book);
    auto apply = [&](BookEdge& edge) {
        if (edge.quality != quality)
            edge.outSize = 0;
        edge.quality = quality;
    };
    apply(tipQuality_[book]);
#ifndef EDGY_XAHAU
    if (book.domain)
        return;
#endif
    apply(tokenTip_[xrpl::PathAsset{book.in}][book.out]);
}

void
LocalOrderBooks::noteOfferUnlocked(
    xrpl::Book const& book,
    std::uint64_t quality,
    double outSize)
{
    addBookUnlocked(book);
    auto apply = [&](BookEdge& edge) {
        if (quality < edge.quality)
        {
            edge.quality = quality;
            edge.outSize = outSize;
        }
        else if (quality == edge.quality && outSize > edge.outSize)
        {
            edge.outSize = outSize;
        }
    };
    apply(tipQuality_[book]);
#ifndef EDGY_XAHAU
    if (book.domain)
        return;
#endif
    apply(tokenTip_[xrpl::PathAsset{book.in}][book.out]);
}

void
LocalOrderBooks::markAmmUnlocked(xrpl::Book const& book)
{
    addBookUnlocked(book);
    tipQuality_[book].amm = true;
#ifndef EDGY_XAHAU
    if (book.domain)
        return;
#endif
    tokenTip_[xrpl::PathAsset{book.in}][book.out].amm = true;
}

void
LocalOrderBooks::forgetOfferUnlocked(xrpl::Book const& book, std::uint64_t quality)
{
    auto forget = [&](BookEdge& edge) {
        if (edge.quality == quality)
            edge.outSize = 0;
    };
    if (auto it = tipQuality_.find(book); it != tipQuality_.end())
        forget(it->second);
#ifndef EDGY_XAHAU
    if (book.domain)
        return;
#endif
    if (auto tit = tokenTip_.find(xrpl::PathAsset{book.in}); tit != tokenTip_.end())
    {
        if (auto oit = tit->second.find(book.out); oit != tit->second.end())
            forget(oit->second);
    }
}

void
LocalOrderBooks::removeBookUnlocked(xrpl::Book const& book)
{
#ifndef EDGY_XAHAU
    if (book.domain)
    {
        if (auto it = domainBooks_.find({book.in, *book.domain}); it != domainBooks_.end())
        {
            it->second.erase(book.out);
            if (it->second.empty())
                domainBooks_.erase(it);
        }
        if (auto it = reverseDomainBooks_.find({book.out, *book.domain});
            it != reverseDomainBooks_.end())
        {
            it->second.erase(book.in);
            if (it->second.empty())
                reverseDomainBooks_.erase(it);
        }
        if (xrpl::isXRP(book.out))
            xrpDomainBooks_.erase({book.in, *book.domain});
        tipQuality_.erase(book);
        return;
    }
#endif

    if (auto it = allBooks_.find(book.in); it != allBooks_.end())
    {
        it->second.erase(book.out);
        if (it->second.empty())
            allBooks_.erase(it);
    }
    if (auto it = reverseBooks_.find(book.out); it != reverseBooks_.end())
    {
        it->second.erase(book.in);
        if (it->second.empty())
            reverseBooks_.erase(it);
    }
    if (auto it = tokenFwd_.find(xrpl::PathAsset{book.in}); it != tokenFwd_.end())
    {
        it->second.erase(book.out);
        if (it->second.empty())
            tokenFwd_.erase(it);
    }
    if (auto it = tokenRev_.find(xrpl::PathAsset{book.out}); it != tokenRev_.end())
    {
        it->second.erase(book.in);
        if (it->second.empty())
            tokenRev_.erase(it);
    }
    if (xrpl::isXRP(book.out))
        xrpBooks_.erase(book.in);
    tipQuality_.erase(book);
    if (auto it = tokenTip_.find(xrpl::PathAsset{book.in}); it != tokenTip_.end())
    {
        it->second.erase(book.out);
        if (it->second.empty())
            tokenTip_.erase(it);
    }
}

void
LocalOrderBooks::addOrderBook(xrpl::Book const& book)
{
    std::unique_lock const lock(lock_);
    addBookUnlocked(book);
}

void
LocalOrderBooks::addFromSle(std::shared_ptr<xrpl::SLE const> const& sle)
{
    if (!sle)
        return;

    if (isBookDirRoot(*sle))
    {
        if (auto book = bookFromDir(*sle))
        {
            std::unique_lock const lock(lock_);
            setDirQualityUnlocked(*book, dirQuality(*sle));
        }
        return;
    }

    if (auto book = bookFromOffer(*sle))
    {
        auto const pays = sle->getFieldAmount(xrpl::sfTakerPays);
        auto const gets = sle->getFieldAmount(xrpl::sfTakerGets);
        auto const q = xrpl::getRate(gets, pays);
        std::unique_lock const lock(lock_);
        noteOfferUnlocked(*book, q, amountAsDouble(gets));
        return;
    }

    if (sle->getType() == xrpl::ltAMM)
    {
        if (!sle->isFieldPresent(xrpl::sfLPTokenBalance) ||
            sle->getFieldAmount(xrpl::sfLPTokenBalance).signum() == 0)
            return;
        if (!sle->isFieldPresent(xrpl::sfAsset) || !sle->isFieldPresent(xrpl::sfAsset2))
            return;
        auto const asset1 = (*sle)[xrpl::sfAsset];
        auto const asset2 = (*sle)[xrpl::sfAsset2];
        std::unique_lock const lock(lock_);
        markAmmUnlocked(makeBook(asset1, asset2));
        markAmmUnlocked(makeBook(asset2, asset1));
    }
}

void
LocalOrderBooks::removeFromSle(std::shared_ptr<xrpl::SLE const> const& sle)
{
    if (!sle)
        return;

    if (isBookDirRoot(*sle))
    {
        if (auto book = bookFromDir(*sle))
        {
            std::unique_lock const lock(lock_);
            removeBookUnlocked(*book);
        }
        return;
    }

    if (auto book = bookFromOffer(*sle))
    {
        auto const pays = sle->getFieldAmount(xrpl::sfTakerPays);
        auto const gets = sle->getFieldAmount(xrpl::sfTakerGets);
        auto const q = xrpl::getRate(gets, pays);
        std::unique_lock const lock(lock_);
        forgetOfferUnlocked(*book, q);
        return;
    }

    if (sle->getType() == xrpl::ltAMM)
    {
        if (!sle->isFieldPresent(xrpl::sfAsset) || !sle->isFieldPresent(xrpl::sfAsset2))
            return;
        auto const asset1 = (*sle)[xrpl::sfAsset];
        auto const asset2 = (*sle)[xrpl::sfAsset2];
        std::unique_lock const lock(lock_);
        removeBookUnlocked(makeBook(asset1, asset2));
        removeBookUnlocked(makeBook(asset2, asset1));
    }
}

void
LocalOrderBooks::clear()
{
    std::unique_lock const lock(lock_);
    allBooks_.clear();
    reverseBooks_.clear();
    domainBooks_.clear();
    reverseDomainBooks_.clear();
    xrpBooks_.clear();
    xrpDomainBooks_.clear();
    tokenFwd_.clear();
    tokenRev_.clear();
    tipQuality_.clear();
    tokenTip_.clear();
}

void
LocalOrderBooks::setup(std::shared_ptr<xrpl::ReadView const> const& ledger)
{
    decltype(allBooks_) allBooks;
    decltype(reverseBooks_) reverseBooks;
    decltype(xrpBooks_) xrpBooks;
    decltype(domainBooks_) domainBooks;
    decltype(reverseDomainBooks_) reverseDomainBooks;
    decltype(xrpDomainBooks_) xrpDomainBooks;
    decltype(tokenFwd_) tokenFwd;
    decltype(tokenRev_) tokenRev;
    decltype(tipQuality_) tipQuality;
    decltype(tokenTip_) tokenTip;

    auto addAdj = [&](xrpl::Book const& book) {
#ifndef EDGY_XAHAU
        if (book.domain)
        {
            domainBooks[{book.in, *book.domain}].insert(book.out);
            reverseDomainBooks[{book.out, *book.domain}].insert(book.in);
            if (xrpl::isXRP(book.out))
                xrpDomainBooks.insert({book.in, *book.domain});
            return;
        }
#endif
        allBooks[book.in].insert(book.out);
        reverseBooks[book.out].insert(book.in);
        tokenFwd[xrpl::PathAsset{book.in}].insert(book.out);
        tokenRev[xrpl::PathAsset{book.out}].insert(book.in);
        if (xrpl::isXRP(book.out))
            xrpBooks.insert(book.in);
    };

    auto setDir = [&](xrpl::Book const& book, std::uint64_t q) {
        addAdj(book);
        auto& edge = tipQuality[book];
        if (edge.quality != q)
            edge.outSize = 0;
        edge.quality = q;
#ifndef EDGY_XAHAU
        if (book.domain)
            return;
#endif
        auto& tok = tokenTip[xrpl::PathAsset{book.in}][book.out];
        if (tok.quality != q)
            tok.outSize = 0;
        tok.quality = q;
    };

    auto noteOffer = [&](xrpl::Book const& book, std::uint64_t q, double size) {
        addAdj(book);
        auto apply = [&](BookEdge& edge) {
            if (q < edge.quality)
            {
                edge.quality = q;
                edge.outSize = size;
            }
            else if (q == edge.quality && size > edge.outSize)
            {
                edge.outSize = size;
            }
        };
        apply(tipQuality[book]);
#ifndef EDGY_XAHAU
        if (book.domain)
            return;
#endif
        apply(tokenTip[xrpl::PathAsset{book.in}][book.out]);
    };

    auto markAmm = [&](xrpl::Book const& book) {
        addAdj(book);
        tipQuality[book].amm = true;
#ifndef EDGY_XAHAU
        if (book.domain)
            return;
#endif
        tokenTip[xrpl::PathAsset{book.in}][book.out].amm = true;
    };

    if (ledger)
    {
        for (auto const& sle : ledger->sles)
        {
            if (isBookDirRoot(*sle))
            {
                if (auto book = bookFromDir(*sle))
                    setDir(*book, dirQuality(*sle));
            }
            else if (auto book = bookFromOffer(*sle))
            {
                auto const pays = sle->getFieldAmount(xrpl::sfTakerPays);
                auto const gets = sle->getFieldAmount(xrpl::sfTakerGets);
                noteOffer(*book, xrpl::getRate(gets, pays), amountAsDouble(gets));
            }
            else if (sle->getType() == xrpl::ltAMM)
            {
                if (!sle->isFieldPresent(xrpl::sfAsset) || !sle->isFieldPresent(xrpl::sfAsset2))
                    continue;
                if (sle->isFieldPresent(xrpl::sfLPTokenBalance) &&
                    sle->getFieldAmount(xrpl::sfLPTokenBalance).signum() == 0)
                    continue;
                auto const asset1 = (*sle)[xrpl::sfAsset];
                auto const asset2 = (*sle)[xrpl::sfAsset2];
                markAmm(makeBook(asset1, asset2));
                markAmm(makeBook(asset2, asset1));
            }
        }
    }

    std::unique_lock const lock(lock_);
    allBooks_.swap(allBooks);
    reverseBooks_.swap(reverseBooks);
    xrpBooks_.swap(xrpBooks);
    domainBooks_.swap(domainBooks);
    reverseDomainBooks_.swap(reverseDomainBooks);
    xrpDomainBooks_.swap(xrpDomainBooks);
    tokenFwd_.swap(tokenFwd);
    tokenRev_.swap(tokenRev);
    tipQuality_.swap(tipQuality);
    tokenTip_.swap(tokenTip);
}

std::vector<xrpl::Book>
LocalOrderBooks::getBooksByTakerPays(
    xrpl::Asset const& asset,
    std::optional<xrpl::Domain> const& domain)
{
    std::vector<xrpl::Book> ret;
    std::shared_lock const lock(lock_);
    auto fill = [&](auto const& container, auto const& key) {
        if (auto it = container.find(key); it != container.end())
        {
            ret.reserve(it->second.size());
            for (auto const& gets : it->second)
                ret.push_back(makeBook(asset, gets, domain));
        }
    };
    if (!domain)
        fill(allBooks_, asset);
    else
        fill(domainBooks_, std::make_pair(asset, *domain));
    return ret;
}

int
LocalOrderBooks::getBookSize(xrpl::Asset const& asset, std::optional<xrpl::Domain> const& domain)
{
    std::shared_lock const lock(lock_);
    if (!domain)
    {
        if (auto it = allBooks_.find(asset); it != allBooks_.end())
            return static_cast<int>(it->second.size());
    }
    else if (auto it = domainBooks_.find({asset, *domain}); it != domainBooks_.end())
    {
        return static_cast<int>(it->second.size());
    }
    return 0;
}

bool
LocalOrderBooks::isBookToXRP(xrpl::Asset const& asset, std::optional<xrpl::Domain> const& domain)
{
    std::shared_lock const lock(lock_);
    if (domain)
        return xrpDomainBooks_.contains({asset, *domain});
    return xrpBooks_.contains(asset);
}

bool
LocalOrderBooks::hasBook(
    xrpl::Asset const& in,
    xrpl::Asset const& out,
    std::optional<xrpl::Domain> const& domain) const
{
    std::shared_lock const lock(lock_);
    if (!domain)
    {
        if (auto const it = allBooks_.find(in); it != allBooks_.end() && it->second.contains(out))
            return true;
        if (auto const it = tokenFwd_.find(xrpl::PathAsset{in});
            it != tokenFwd_.end() && it->second.contains(out))
            return true;
        return false;
    }
    auto const it = domainBooks_.find({in, *domain});
    return it != domainBooks_.end() && it->second.contains(out);
}

std::vector<xrpl::Asset>
LocalOrderBooks::intermediates(
    xrpl::Asset const& src,
    xrpl::Asset const& dst,
    std::optional<xrpl::Domain> const& domain) const
{
    std::shared_lock const lock(lock_);
    xrpl::hardened_hash_set<xrpl::Asset> fwdSet;
    xrpl::hardened_hash_set<xrpl::Asset> revSet;
    xrpl::hardened_hash_set<xrpl::Asset> const* fwd = nullptr;
    xrpl::hardened_hash_set<xrpl::Asset> const* rev = nullptr;
    if (!domain)
    {
        if (auto it = allBooks_.find(src); it != allBooks_.end())
            fwdSet.insert(it->second.begin(), it->second.end());
        if (auto it = tokenFwd_.find(xrpl::PathAsset{src}); it != tokenFwd_.end())
            fwdSet.insert(it->second.begin(), it->second.end());
        if (!fwdSet.empty())
            fwd = &fwdSet;
        if (auto it = reverseBooks_.find(dst); it != reverseBooks_.end())
            revSet.insert(it->second.begin(), it->second.end());
        if (auto it = tokenRev_.find(xrpl::PathAsset{dst}); it != tokenRev_.end())
            revSet.insert(it->second.begin(), it->second.end());
        if (!revSet.empty())
            rev = &revSet;
    }
    else
    {
        if (auto it = domainBooks_.find({src, *domain}); it != domainBooks_.end())
            fwd = &it->second;
        if (auto it = reverseDomainBooks_.find({dst, *domain}); it != reverseDomainBooks_.end())
            rev = &it->second;
    }
    if (!fwd || !rev)
        return {};

    auto const* smaller = fwd;
    auto const* larger = rev;
    if (rev->size() < fwd->size())
        std::swap(smaller, larger);

    std::vector<xrpl::Asset> out;
    out.reserve(std::min(smaller->size(), std::size_t{32}));
    for (auto const& asset : *smaller)
    {
        if (larger->contains(asset))
            out.push_back(asset);
    }
    return out;
}

std::vector<xrpl::Asset>
LocalOrderBooks::neighbors(
    xrpl::Asset const& in,
    std::optional<xrpl::Domain> const& domain) const
{
    std::shared_lock const lock(lock_);
    xrpl::hardened_hash_set<xrpl::Asset> merged;
    if (!domain)
    {
        if (auto it = allBooks_.find(in); it != allBooks_.end())
            merged.insert(it->second.begin(), it->second.end());
        if (auto it = tokenFwd_.find(xrpl::PathAsset{in}); it != tokenFwd_.end())
            merged.insert(it->second.begin(), it->second.end());
    }
    else if (auto it = domainBooks_.find({in, *domain}); it != domainBooks_.end())
    {
        merged.insert(it->second.begin(), it->second.end());
    }
    return {merged.begin(), merged.end()};
}

std::vector<xrpl::Asset>
LocalOrderBooks::predecessors(
    xrpl::Asset const& out,
    std::optional<xrpl::Domain> const& domain) const
{
    std::shared_lock const lock(lock_);
    xrpl::hardened_hash_set<xrpl::Asset> merged;
    if (!domain)
    {
        if (auto it = reverseBooks_.find(out); it != reverseBooks_.end())
            merged.insert(it->second.begin(), it->second.end());
        if (auto it = tokenRev_.find(xrpl::PathAsset{out}); it != tokenRev_.end())
            merged.insert(it->second.begin(), it->second.end());
    }
    else if (auto it = reverseDomainBooks_.find({out, *domain}); it != reverseDomainBooks_.end())
    {
        merged.insert(it->second.begin(), it->second.end());
    }
    return {merged.begin(), merged.end()};
}

BookEdge
LocalOrderBooks::tip(
    xrpl::Asset const& in,
    xrpl::Asset const& out,
    std::optional<xrpl::Domain> const& domain) const
{
    std::shared_lock const lock(lock_);
    BookEdge best;
    xrpl::Book const book = makeBook(in, out, domain);
    if (auto it = tipQuality_.find(book); it != tipQuality_.end())
        best = it->second;
    if (!domain)
    {
        if (auto tit = tokenTip_.find(xrpl::PathAsset{in}); tit != tokenTip_.end())
        {
            if (auto oit = tit->second.find(out); oit != tit->second.end())
                mergeBetter(best, oit->second);
        }
    }
    return best;
}

std::uint64_t
LocalOrderBooks::tipQuality(
    xrpl::Asset const& in,
    xrpl::Asset const& out,
    std::optional<xrpl::Domain> const& domain) const
{
    return tip(in, out, domain).quality;
}

std::size_t
LocalOrderBooks::bookCount() const
{
    std::shared_lock const lock(lock_);
    return tipQuality_.size();
}

}  // namespace edgy
