#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Journal.h>

namespace ripple {

class ServiceRegistry
{
public:
    virtual ~ServiceRegistry() = default;
};

}  // namespace ripple
