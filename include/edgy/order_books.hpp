#pragma once

#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/ledger/OrderBookDB.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/Asset.h>
#include <xrpl/protocol/Book.h>
#include <xrpl/protocol/PathAsset.h>
#include <xrpl/protocol/UintTypes.h>

#include <cstdint>
#include <limits>

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace edgy {

/**
 * Cheap edge weight for book-graph search.
 *
 * quality is the packed ExchangeRate / getRate (lower is better).
 * outSize is remaining takerGets at the tip (0 = unknown).
 * amm is true when an AMM also quotes this pair.
 */
struct BookEdge
{
    std::uint64_t quality{std::numeric_limits<std::uint64_t>::max()};
    double outSize{0};
    bool amm{false};
};

/**
 * In-memory OrderBookDB matching xrpld's OrderBookDBImpl lookup semantics.
 *
 * Built from Offer directory roots (sfExchangeRate + root index), Offer
 * objects (tip size), and AMM objects — the same objects xrpld walks in
 * OrderBookDBImpl::update, plus offer size for amount-aware ranking.
 */
class LocalOrderBooks final : public xrpl::OrderBookDB
{
public:
    LocalOrderBooks() = default;

    void
    setup(std::shared_ptr<xrpl::ReadView const> const& ledger) override;

    void
    addOrderBook(xrpl::Book const& book) override;

    std::vector<xrpl::Book>
    getBooksByTakerPays(
        xrpl::Asset const& asset,
        std::optional<xrpl::Domain> const& domain = std::nullopt) override;

    int
    getBookSize(
        xrpl::Asset const& asset,
        std::optional<xrpl::Domain> const& domain = std::nullopt) override;

    bool
    isBookToXRP(
        xrpl::Asset const& asset,
        std::optional<xrpl::Domain> const& domain = std::nullopt) override;

    void
    addFromSle(std::shared_ptr<xrpl::SLE const> const& sle);

    void
    removeFromSle(std::shared_ptr<xrpl::SLE const> const& sle);

    void
    clear();

    [[nodiscard]] bool
    hasBook(
        xrpl::Asset const& in,
        xrpl::Asset const& out,
        std::optional<xrpl::Domain> const& domain = std::nullopt) const;

    // Assets M such that book(src→M) and book(M→dst) both exist.
    [[nodiscard]] std::vector<xrpl::Asset>
    intermediates(
        xrpl::Asset const& src,
        xrpl::Asset const& dst,
        std::optional<xrpl::Domain> const& domain = std::nullopt) const;

    [[nodiscard]] std::vector<xrpl::Asset>
    neighbors(
        xrpl::Asset const& in,
        std::optional<xrpl::Domain> const& domain = std::nullopt) const;

    // Assets IN such that book(IN→out) exists.
    [[nodiscard]] std::vector<xrpl::Asset>
    predecessors(
        xrpl::Asset const& out,
        std::optional<xrpl::Domain> const& domain = std::nullopt) const;

    // Best known book-tip quality (lower uint64 is better). Missing = max.
    [[nodiscard]] std::uint64_t
    tipQuality(
        xrpl::Asset const& in,
        xrpl::Asset const& out,
        std::optional<xrpl::Domain> const& domain = std::nullopt) const;

    [[nodiscard]] BookEdge
    tip(
        xrpl::Asset const& in,
        xrpl::Asset const& out,
        std::optional<xrpl::Domain> const& domain = std::nullopt) const;

    static constexpr std::uint64_t kNoQuality = std::numeric_limits<std::uint64_t>::max();

    [[nodiscard]] std::size_t
    bookCount() const;

private:
    void
    addBookUnlocked(xrpl::Book const& book);

    void
    noteQualityUnlocked(xrpl::Book const& book, std::uint64_t quality);

    void
    setDirQualityUnlocked(xrpl::Book const& book, std::uint64_t quality);

    void
    noteOfferUnlocked(xrpl::Book const& book, std::uint64_t quality, double outSize);

    void
    markAmmUnlocked(xrpl::Book const& book);

    void
    removeBookUnlocked(xrpl::Book const& book);

    void
    forgetOfferUnlocked(xrpl::Book const& book, std::uint64_t quality);

    mutable std::mutex lock_;
    xrpl::hardened_hash_map<xrpl::Asset, xrpl::hardened_hash_set<xrpl::Asset>> allBooks_;
    xrpl::hardened_hash_map<xrpl::Asset, xrpl::hardened_hash_set<xrpl::Asset>> reverseBooks_;
    xrpl::hardened_hash_map<std::pair<xrpl::Asset, xrpl::Domain>, xrpl::hardened_hash_set<xrpl::Asset>>
        domainBooks_;
    xrpl::hardened_hash_map<std::pair<xrpl::Asset, xrpl::Domain>, xrpl::hardened_hash_set<xrpl::Asset>>
        reverseDomainBooks_;
    xrpl::hash_set<xrpl::Asset> xrpBooks_;
    xrpl::hash_set<std::pair<xrpl::Asset, xrpl::Domain>> xrpDomainBooks_;
    // Currency/MPT → outs/ins, so XAH.wallet still sees XAH.gateway books.
    xrpl::hardened_hash_map<xrpl::PathAsset, xrpl::hardened_hash_set<xrpl::Asset>> tokenFwd_;
    xrpl::hardened_hash_map<xrpl::PathAsset, xrpl::hardened_hash_set<xrpl::Asset>> tokenRev_;
    xrpl::hardened_hash_map<xrpl::Book, BookEdge> tipQuality_;
    xrpl::hardened_hash_map<xrpl::PathAsset, xrpl::hardened_hash_map<xrpl::Asset, BookEdge>>
        tokenTip_;
};

}  // namespace edgy
