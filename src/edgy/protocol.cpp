#include <edgy/protocol.hpp>

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STPathSet.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/detail/STVar.h>
#include <xrpl/protocol/jss.h>

#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

namespace edgy {
namespace {

enum class Container
{
    Top,
    Object,
    Array,
};

bool
iequals(std::string const& a, char const* b)
{
    auto p = b;
    for (unsigned char c : a)
    {
        if (*p == '\0' || std::tolower(c) != std::tolower(static_cast<unsigned char>(*p)))
            return false;
        ++p;
    }
    return *p == '\0';
}

bool
isZeroIssuer(json::Value const& obj)
{
    if (!obj.isMember(xrpl::jss::issuer))
        return true;
    if (!obj[xrpl::jss::issuer].isString())
        return false;
    auto const s = obj[xrpl::jss::issuer].asString();
    if (s.empty())
        return true;
    auto const id = xrpl::parseBase58<xrpl::AccountID>(s);
    return id && id->isZero();
}

// {currency: XAH, value: "1"} → "1000000" drops. libxrpl rejects native objects.
bool
xahValueToDrops(json::Value const& value, std::string& out)
{
    std::string s;
    if (value.isString())
        s = value.asString();
    else if (value.isInt())
        s = std::to_string(value.asInt());
    else if (value.isUInt())
        s = std::to_string(value.asUInt());
    else
        return false;
    if (s == "-1")
    {
        out = "-1";
        return true;
    }
    bool neg = false;
    if (!s.empty() && s.front() == '-')
    {
        neg = true;
        s.erase(0, 1);
    }
    if (s.empty())
        return false;
    auto const dot = s.find('.');
    std::string whole = dot == std::string::npos ? s : s.substr(0, dot);
    std::string frac = dot == std::string::npos ? std::string{} : s.substr(dot + 1);
    if (frac.size() > 6)
        return false;
    for (unsigned char c : whole)
    {
        if (!std::isdigit(c))
            return false;
    }
    for (unsigned char c : frac)
    {
        if (!std::isdigit(c))
            return false;
    }
    while (frac.size() < 6)
        frac.push_back('0');
    if (whole.empty())
        whole = "0";
    auto drops = whole + frac;
    auto i = drops.find_first_not_of('0');
    if (i == std::string::npos)
        drops = "0";
    else
        drops = drops.substr(i);
    if (neg && drops != "0")
        drops.insert(drops.begin(), '-');
    out = std::move(drops);
    return true;
}

void
copyBytes(xrpl::SerialIter& sit, xrpl::Serializer* out, int n)
{
    auto const raw = sit.getRaw(n);
    if (out)
        out->addRaw(raw);
}

void
copyOrSkipPayload(xrpl::SerialIter& sit, xrpl::Serializer* out, int type);

void
walk(xrpl::SerialIter& sit, xrpl::Serializer* out, Container kind)
{
    while (!sit.empty())
    {
        int type = 0;
        int field = 0;
        sit.getFieldID(type, field);
        if (type == xrpl::STI_OBJECT && field == 1)
        {
            if (kind == Container::Object && out)
                out->addFieldID(type, field);
            if (kind == Container::Object)
                return;
            continue;
        }
        if (type == xrpl::STI_ARRAY && field == 1)
        {
            if (kind == Container::Array && out)
                out->addFieldID(type, field);
            if (kind == Container::Array)
                return;
            continue;
        }
        auto const& fn = xrpl::SField::getField(type, field);
        bool const keep = out && !fn.isInvalid();
        if (keep)
            out->addFieldID(type, field);
        copyOrSkipPayload(sit, keep ? out : nullptr, type);
    }
}

void
copyOrSkipPayload(xrpl::SerialIter& sit, xrpl::Serializer* out, int type)
{
    switch (type)
    {
        case xrpl::STI_UINT8:
            copyBytes(sit, out, 1);
            return;
        case xrpl::STI_UINT16:
            copyBytes(sit, out, 2);
            return;
        case xrpl::STI_UINT32:
        case xrpl::STI_INT32:
            copyBytes(sit, out, 4);
            return;
        case xrpl::STI_UINT64:
        case xrpl::STI_INT64:
            copyBytes(sit, out, 8);
            return;
        case xrpl::STI_UINT96:
            copyBytes(sit, out, 12);
            return;
        case xrpl::STI_UINT128:
            copyBytes(sit, out, 16);
            return;
        case xrpl::STI_UINT160:
        case xrpl::STI_CURRENCY:
            copyBytes(sit, out, 20);
            return;
        case xrpl::STI_UINT192:
            copyBytes(sit, out, 24);
            return;
        case xrpl::STI_UINT256:
            copyBytes(sit, out, 32);
            return;
        case xrpl::STI_UINT384:
            copyBytes(sit, out, 48);
            return;
        case xrpl::STI_UINT512:
            copyBytes(sit, out, 64);
            return;
        case xrpl::STI_NUMBER:
            copyBytes(sit, out, 12);
            return;
        case xrpl::STI_VL:
        case xrpl::STI_ACCOUNT:
        case xrpl::STI_VECTOR256: {
            auto const data = sit.getVL();
            if (out)
                out->addVL(data.data(), static_cast<int>(data.size()));
            return;
        }
        case xrpl::STI_AMOUNT: {
            auto const value = sit.get64();
            if (out)
                out->add64(value);
            if ((value & xrpl::STAmount::kIssuedCurrency) == 0)
            {
                if ((value & xrpl::STAmount::kMpToken) != 0)
                {
                    auto const extra = sit.get8();
                    auto const mpt = sit.get192();
                    if (out)
                    {
                        out->add8(extra);
                        out->addBitString(mpt);
                    }
                }
                return;
            }
            auto const currency = sit.get160();
            auto const issuer = sit.get160();
            if (out)
            {
                out->addBitString(currency);
                out->addBitString(issuer);
            }
            return;
        }
        case xrpl::STI_ISSUE: {
            auto const first = sit.get160();
            if (out)
                out->addBitString(first);
            if (first.isZero())
                return;
            auto const account = sit.get160();
            if (out)
                out->addBitString(account);
            if (account.isZero())
            {
                auto const seq = sit.get32();
                if (out)
                    out->add32(seq);
            }
            return;
        }
        case xrpl::STI_OBJECT:
            walk(sit, out, Container::Object);
            return;
        case xrpl::STI_ARRAY:
            walk(sit, out, Container::Array);
            return;
        case xrpl::STI_PATHSET: {
            for (;;)
            {
                auto const t = sit.get8();
                if (out)
                    out->add8(t);
                if (t == xrpl::STPathElement::TypeNone)
                    return;
                if (t == xrpl::STPathElement::TypeBoundary)
                    continue;
                if ((t & xrpl::STPathElement::TypeAccount) != 0)
                    copyBytes(sit, out, 20);
                if ((t & xrpl::STPathElement::TypeCurrency) != 0)
                    copyBytes(sit, out, 20);
                if ((t & xrpl::STPathElement::TypeMpt) != 0)
                    copyBytes(sit, out, 24);
                if ((t & xrpl::STPathElement::TypeIssuer) != 0)
                    copyBytes(sit, out, 20);
            }
        }
        case xrpl::STI_XCHAIN_BRIDGE:
            for (int i = 0; i < 4; ++i)
            {
                int innerType = 0;
                int innerField = 0;
                sit.getFieldID(innerType, innerField);
                if (out)
                    out->addFieldID(innerType, innerField);
                copyOrSkipPayload(sit, out, innerType);
            }
            return;
        default:
            throw std::runtime_error("unsupported serialized type while stripping fields");
    }
}

xrpl::Blob
stripUnknownFields(xrpl::Blob const& blob)
{
    xrpl::Serializer kept;
    xrpl::SerialIter sit(xrpl::makeSlice(blob));
    walk(sit, &kept, Container::Top);
    return kept.getData();
}

xrpl::SLE::pointer
sleFromKnownFields(xrpl::Blob const& blob, xrpl::uint256 const& key)
{
    xrpl::SerialIter sit(xrpl::makeSlice(blob));
    xrpl::STObject obj(xrpl::sfGeneric);
    obj.set(sit);
    if (!obj.isFieldPresent(xrpl::sfLedgerEntryType))
        return nullptr;
    auto const type = static_cast<xrpl::LedgerEntryType>(
        obj.getFieldU16(xrpl::sfLedgerEntryType));
    auto const* format = xrpl::LedgerFormats::getInstance().findByType(type);
    if (!format)
        return nullptr;
    auto sle = std::make_shared<xrpl::SLE>(type, key);
    for (auto const& e : format->getSOTemplate())
    {
        auto const& field = e.sField();
        if (field == xrpl::sfLedgerEntryType || !obj.isFieldPresent(field))
            continue;
        xrpl::detail::STVar var(obj.peekAtField(field));
        sle->set(std::move(var.get()));
    }
    return sle;
}

void
rewriteNativeWalk(json::Value& v, bool inbound, bool xahau)
{
    if (!xahau)
        return;
    if (v.isObject())
    {
        if (v.isMember(xrpl::jss::currency) && v[xrpl::jss::currency].isString() &&
            isZeroIssuer(v))
        {
            auto const code = v[xrpl::jss::currency].asString();
            if (inbound && iequals(code, "XAH"))
            {
                if (v.isMember(xrpl::jss::value))
                {
                    std::string drops;
                    if (xahValueToDrops(v[xrpl::jss::value], drops))
                    {
                        v = drops;
                        return;
                    }
                }
                v[xrpl::jss::currency] = "XRP";
            }
            else if (!inbound && iequals(code, "XRP"))
            {
                v[xrpl::jss::currency] = "XAH";
            }
        }
        for (auto const& name : v.getMemberNames())
        {
            auto& child = v[name];
            bool const currencyList =
                name == "destination_currencies" || name == "source_currencies";
            if (child.isArray() && currencyList)
            {
                for (unsigned i = 0; i < child.size(); ++i)
                {
                    if (child[i].isString())
                    {
                        auto const& code = child[i].asString();
                        if (inbound && iequals(code, "XAH"))
                            child[i] = "XRP";
                        else if (!inbound && iequals(code, "XRP"))
                            child[i] = "XAH";
                    }
                    else
                    {
                        rewriteNativeWalk(child[i], inbound, true);
                    }
                }
            }
            else
            {
                rewriteNativeWalk(child, inbound, true);
            }
        }
        return;
    }
    if (v.isArray())
    {
        for (unsigned i = 0; i < v.size(); ++i)
            rewriteNativeWalk(v[i], inbound, true);
    }
}

}  // namespace

xrpl::SLE::pointer
sleFromBlob(xrpl::Blob const& blob, xrpl::uint256 const& key)
{
    auto tryParse = [&](xrpl::Blob const& data) -> xrpl::SLE::pointer {
        try
        {
            xrpl::SerialIter sit(xrpl::makeSlice(data));
            return std::make_shared<xrpl::SLE>(sit, key);
        }
        catch (...)
        {
            return nullptr;
        }
    };

    if (auto sle = tryParse(blob))
        return sle;

    xrpl::Blob stripped;
    try
    {
        stripped = stripUnknownFields(blob);
    }
    catch (...)
    {
        return nullptr;
    }
    if (auto sle = tryParse(stripped))
        return sle;
    try
    {
        return sleFromKnownFields(stripped, key);
    }
    catch (...)
    {
        return nullptr;
    }
}

xrpl::SLE::pointer
sleFromBinary(std::string const& dataHex, std::string const& indexHex)
{
    auto const blob = xrpl::strUnHex(dataHex);
    if (!blob)
        return nullptr;
    xrpl::uint256 key;
    if (!key.parseHex(indexHex))
        return nullptr;
    return sleFromBlob(*blob, key);
}

void
stripUnknownJsonFields(json::Value& v)
{
    if (v.isObject())
    {
        auto const names = v.getMemberNames();
        for (auto const& name : names)
        {
            if (xrpl::SField::getField(name).isInvalid())
            {
                v.removeMember(name);
                continue;
            }
            stripUnknownJsonFields(v[name]);
        }
        return;
    }
    if (v.isArray())
    {
        for (unsigned i = 0; i < v.size(); ++i)
            stripUnknownJsonFields(v[i]);
    }
}

void
rewriteNativeJsonIn(json::Value& v, NetworkKind network)
{
    rewriteNativeWalk(v, true, network == NetworkKind::xahau);
}

void
rewriteNativeJsonOut(json::Value& v, NetworkKind network)
{
    rewriteNativeWalk(v, false, network == NetworkKind::xahau);
}

}  // namespace edgy
