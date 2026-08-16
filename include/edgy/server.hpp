#pragma once

#include <edgy/config.hpp>
#include <edgy/engine.hpp>
#include <edgy/node_client.hpp>

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace edgy {

/**
 * HTTP JSON-RPC + WebSocket front end.
 *
 * path_find / ripple_path_find are answered from Engine (local memory).
 * Every other command is proxied to the upstream xrpld/xahaud so the wire
 * envelope matches a real node.
 */
class Server
{
public:
    Server(boost::asio::io_context& io, Config cfg, Engine& engine, std::shared_ptr<NodeClient> node);
    ~Server();

    void
    start();

    void
    stop();

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace edgy
