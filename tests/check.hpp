#pragma once

#include <xrpl/protocol/AccountID.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace edgy::test {

inline int gFails = 0;

inline void
expect(bool cond, char const* what)
{
    if (!cond)
    {
        std::cerr << "FAIL: " << what << '\n';
        ++gFails;
    }
    else
    {
        std::cerr << "ok    " << what << '\n';
    }
}

inline xrpl::AccountID
testAccount(char const* b58)
{
    auto id = xrpl::parseBase58<xrpl::AccountID>(b58);
    if (!id)
        throw std::runtime_error(std::string("bad account ") + b58);
    return *id;
}

inline int
finish(char const* suite)
{
    if (gFails != 0)
    {
        std::cerr << suite << ": " << gFails << " test(s) failed\n";
        return 1;
    }
    std::cerr << suite << ": all tests passed\n";
    return 0;
}

}  // namespace edgy::test
