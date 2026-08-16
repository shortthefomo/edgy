#include <pathfinder/order_books.hpp>

#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/PathAsset.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/UintTypes.h>

#include <algorithm>
#include <utility>

namespace pathfinder {

void
LocalOrderBooks::addBookUnlocked(xrpl::Book const& book)
{
    if (book.domain)
    {
        domainBooks_[{book.in, *book.domain}].insert(book.out);
        reverseDomainBooks_[{book.out, *book.domain}].insert(book.in);
        if (xrpl::isXRP(book.out))
            xrpDomainBooks_.insert({book.in, *book.domain});
        return;
    }

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
    setQualityUnlocked(book, quality);
}

void
LocalOrderBooks::setQualityUnlocked(xrpl::Book const& book, std::uint64_t quality)
{
    tipQuality_[book] = quality;
    tokenTip_[xrpl::PathAsset{book.in}][book.out] = quality;
}

void
LocalOrderBooks::removeBookUnlocked(xrpl::Book const& book)
{
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
    std::lock_guard const lock(lock_);
    addBookUnlocked(book);
}

void
LocalOrderBooks::addFromSle(xrpl::SLE::const_ref sle)
{
    if (!sle)
        return;

    if (sle->getType() == xrpl::ltDIR_NODE && sle->isFieldPresent(xrpl::sfExchangeRate) &&
        sle->getFieldH256(xrpl::sfRootIndex) == sle->key())
    {
        xrpl::Book book;
        if (sle->isFieldPresent(xrpl::sfTakerPaysCurrency))
        {
            xrpl::Issue issue;
            issue.currency = sle->getFieldH160(xrpl::sfTakerPaysCurrency);
            issue.account = sle->getFieldH160(xrpl::sfTakerPaysIssuer);
            book.in = issue;
        }
        else if (sle->isFieldPresent(xrpl::sfTakerPaysMPT))
        {
            book.in = sle->getFieldH192(xrpl::sfTakerPaysMPT);
        }
        else
        {
            return;
        }

        if (sle->isFieldPresent(xrpl::sfTakerGetsCurrency))
        {
            xrpl::Issue issue;
            issue.currency = sle->getFieldH160(xrpl::sfTakerGetsCurrency);
            issue.account = sle->getFieldH160(xrpl::sfTakerGetsIssuer);
            book.out = issue;
        }
        else if (sle->isFieldPresent(xrpl::sfTakerGetsMPT))
        {
            book.out = sle->getFieldH192(xrpl::sfTakerGetsMPT);
        }
        else
        {
            return;
        }
        book.domain = (*sle)[~xrpl::sfDomainID];
        addOrderBook(book);
        std::uint64_t q = xrpl::getQuality(sle->key());
        if (sle->isFieldPresent(xrpl::sfExchangeRate))
            q = sle->getFieldU64(xrpl::sfExchangeRate);
        {
            std::lock_guard const lock(lock_);
            setQualityUnlocked(book, q);
        }
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
        addOrderBook({asset1, asset2, std::nullopt});
        addOrderBook({asset2, asset1, std::nullopt});
    }
}

void
LocalOrderBooks::removeFromSle(xrpl::SLE::const_ref sle)
{
    if (!sle)
        return;

    if (sle->getType() == xrpl::ltDIR_NODE && sle->isFieldPresent(xrpl::sfExchangeRate) &&
        sle->isFieldPresent(xrpl::sfRootIndex) && sle->getFieldH256(xrpl::sfRootIndex) == sle->key())
    {
        xrpl::Book book;
        if (sle->isFieldPresent(xrpl::sfTakerPaysCurrency))
        {
            xrpl::Issue issue;
            issue.currency = sle->getFieldH160(xrpl::sfTakerPaysCurrency);
            issue.account = sle->getFieldH160(xrpl::sfTakerPaysIssuer);
            book.in = issue;
        }
        else if (sle->isFieldPresent(xrpl::sfTakerPaysMPT))
        {
            book.in = sle->getFieldH192(xrpl::sfTakerPaysMPT);
        }
        else
        {
            return;
        }
        if (sle->isFieldPresent(xrpl::sfTakerGetsCurrency))
        {
            xrpl::Issue issue;
            issue.currency = sle->getFieldH160(xrpl::sfTakerGetsCurrency);
            issue.account = sle->getFieldH160(xrpl::sfTakerGetsIssuer);
            book.out = issue;
        }
        else if (sle->isFieldPresent(xrpl::sfTakerGetsMPT))
        {
            book.out = sle->getFieldH192(xrpl::sfTakerGetsMPT);
        }
        else
        {
            return;
        }
        book.domain = (*sle)[~xrpl::sfDomainID];
        std::lock_guard const lock(lock_);
        removeBookUnlocked(book);
        return;
    }

    if (sle->getType() == xrpl::ltAMM)
    {
        if (!sle->isFieldPresent(xrpl::sfAsset) || !sle->isFieldPresent(xrpl::sfAsset2))
            return;
        auto const asset1 = (*sle)[xrpl::sfAsset];
        auto const asset2 = (*sle)[xrpl::sfAsset2];
        std::lock_guard const lock(lock_);
        removeBookUnlocked({asset1, asset2, std::nullopt});
        removeBookUnlocked({asset2, asset1, std::nullopt});
    }
}

void
LocalOrderBooks::clear()
{
    std::lock_guard const lock(lock_);
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

    if (ledger)
    {
        for (auto const& sle : ledger->sles)
        {
            if (sle->getType() == xrpl::ltDIR_NODE && sle->isFieldPresent(xrpl::sfExchangeRate) &&
                sle->getFieldH256(xrpl::sfRootIndex) == sle->key())
            {
                xrpl::Book book;
                if (sle->isFieldPresent(xrpl::sfTakerPaysCurrency))
                {
                    xrpl::Issue issue;
                    issue.currency = sle->getFieldH160(xrpl::sfTakerPaysCurrency);
                    issue.account = sle->getFieldH160(xrpl::sfTakerPaysIssuer);
                    book.in = issue;
                }
                else if (sle->isFieldPresent(xrpl::sfTakerPaysMPT))
                {
                    book.in = sle->getFieldH192(xrpl::sfTakerPaysMPT);
                }
                else
                {
                    continue;
                }
                if (sle->isFieldPresent(xrpl::sfTakerGetsCurrency))
                {
                    xrpl::Issue issue;
                    issue.currency = sle->getFieldH160(xrpl::sfTakerGetsCurrency);
                    issue.account = sle->getFieldH160(xrpl::sfTakerGetsIssuer);
                    book.out = issue;
                }
                else if (sle->isFieldPresent(xrpl::sfTakerGetsMPT))
                {
                    book.out = sle->getFieldH192(xrpl::sfTakerGetsMPT);
                }
                else
                {
                    continue;
                }
                book.domain = (*sle)[~xrpl::sfDomainID];
                if (book.domain)
                {
                    domainBooks[{book.in, *book.domain}].insert(book.out);
                    reverseDomainBooks[{book.out, *book.domain}].insert(book.in);
                    if (xrpl::isXRP(book.out))
                        xrpDomainBooks.insert({book.in, *book.domain});
                }
                else
                {
                    allBooks[book.in].insert(book.out);
                    reverseBooks[book.out].insert(book.in);
                    tokenFwd[xrpl::PathAsset{book.in}].insert(book.out);
                    tokenRev[xrpl::PathAsset{book.out}].insert(book.in);
                    if (xrpl::isXRP(book.out))
                        xrpBooks.insert(book.in);
                    std::uint64_t q = xrpl::getQuality(sle->key());
                    if (sle->isFieldPresent(xrpl::sfExchangeRate))
                        q = sle->getFieldU64(xrpl::sfExchangeRate);
                    auto [it, ins] = tipQuality.emplace(book, q);
                    if (!ins && q < it->second)
                        it->second = q;
                    auto& byOut = tokenTip[xrpl::PathAsset{book.in}];
                    auto [tit, tins] = byOut.emplace(book.out, q);
                    if (!tins && q < tit->second)
                        tit->second = q;
                }
            }
            else if (sle->getType() == xrpl::ltAMM)
            {
                auto const asset1 = (*sle)[xrpl::sfAsset];
                auto const asset2 = (*sle)[xrpl::sfAsset2];
                allBooks[asset1].insert(asset2);
                allBooks[asset2].insert(asset1);
                reverseBooks[asset2].insert(asset1);
                reverseBooks[asset1].insert(asset2);
                tokenFwd[xrpl::PathAsset{asset1}].insert(asset2);
                tokenFwd[xrpl::PathAsset{asset2}].insert(asset1);
                tokenRev[xrpl::PathAsset{asset2}].insert(asset1);
                tokenRev[xrpl::PathAsset{asset1}].insert(asset2);
                if (xrpl::isXRP(asset2))
                    xrpBooks.insert(asset1);
                if (xrpl::isXRP(asset1))
                    xrpBooks.insert(asset2);
            }
        }
    }

    std::lock_guard const lock(lock_);
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
    std::lock_guard const lock(lock_);
    auto fill = [&](auto const& container, auto const& key) {
        if (auto it = container.find(key); it != container.end())
        {
            ret.reserve(it->second.size());
            for (auto const& gets : it->second)
                ret.emplace_back(asset, gets, domain);
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
    std::lock_guard const lock(lock_);
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
    std::lock_guard const lock(lock_);
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
    std::lock_guard const lock(lock_);
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
    std::lock_guard const lock(lock_);
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
    std::lock_guard const lock(lock_);
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

std::uint64_t
LocalOrderBooks::tipQuality(
    xrpl::Asset const& in,
    xrpl::Asset const& out,
    std::optional<xrpl::Domain> const& domain) const
{
    std::lock_guard const lock(lock_);
    std::uint64_t best = kNoQuality;
    xrpl::Book const book{in, out, domain};
    if (auto it = tipQuality_.find(book); it != tipQuality_.end())
        best = it->second;
    if (auto tit = tokenTip_.find(xrpl::PathAsset{in}); tit != tokenTip_.end())
    {
        if (auto oit = tit->second.find(out); oit != tit->second.end() && oit->second < best)
            best = oit->second;
    }
    return best;
}

std::size_t
LocalOrderBooks::bookCount() const
{
    std::lock_guard const lock(lock_);
    return tipQuality_.size();
}

}  // namespace pathfinder
