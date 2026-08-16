#pragma once

#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/Asset.h>
#include <xrpl/protocol/Book.h>
#include <xrpl/protocol/UintTypes.h>

#include <memory>
#include <optional>
#include <vector>

namespace ripple {

using Domain = uint256;

class OrderBookDB
{
public:
    virtual ~OrderBookDB() = default;

    virtual void
    setup(std::shared_ptr<ReadView const> const&)
    {
    }

    virtual void
    addOrderBook(Book const&)
    {
    }

    virtual std::vector<Book>
    getBooksByTakerPays(Asset const&, std::optional<Domain> const& = std::nullopt)
    {
        return {};
    }

    virtual int
    getBookSize(Asset const&, std::optional<Domain> const& = std::nullopt)
    {
        return 0;
    }

    virtual bool
    isBookToXRP(Asset const&, std::optional<Domain> const& = std::nullopt)
    {
        return false;
    }
};

}  // namespace ripple
