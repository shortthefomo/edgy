#pragma once

#include <xrpl/protocol/Asset.h>
#include <xrpl/protocol/UintTypes.h>

#include <stdexcept>
#include <string>
#include <type_traits>

namespace ripple {

// xahaud paths are currency-only (no PathAsset / MPT hop type).
class PathAsset
{
    Currency currency_{};

public:
    PathAsset() = default;

    PathAsset(Currency const& c) : currency_(c)
    {
    }

    PathAsset(Asset const& asset)
    {
        if (asset.holds<Issue>())
            currency_ = asset.get<Issue>().currency;
    }

    PathAsset(MPTID const&)
    {
    }

    template <class T>
    constexpr bool
    holds() const
    {
        return std::is_same_v<T, Currency>;
    }

    template <class T>
    T const&
    get() const
    {
        if constexpr (std::is_same_v<T, Currency>)
            return currency_;
        throw std::logic_error("PathAsset on xahaud only holds Currency");
    }

    constexpr bool
    isXRP() const
    {
        return ripple::isXRP(currency_);
    }

    template <typename Cvis, typename Mvis>
    auto
    visit(Cvis&& cvis, Mvis&&) const
    {
        return cvis(currency_);
    }

    friend bool
    operator==(PathAsset const& a, PathAsset const& b)
    {
        return a.currency_ == b.currency_;
    }

    friend bool
    operator==(PathAsset const& a, Asset const& b)
    {
        return b.holds<Issue>() && b.get<Issue>().currency == a.currency_;
    }

    friend bool
    operator==(Asset const& a, PathAsset const& b)
    {
        return b == a;
    }
};

inline bool
isXRP(PathAsset const& a)
{
    return a.isXRP();
}

inline std::string
to_string(PathAsset const& pathAsset)
{
    return to_string(pathAsset.template get<Currency>());
}

template <typename Hasher>
void
hash_append(Hasher& h, PathAsset const& pathAsset)
{
    hash_append(h, pathAsset.template get<Currency>());
}

}  // namespace ripple
