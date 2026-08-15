#pragma once

#include <pathfinder/config.hpp>
#include <pathfinder/engine.hpp>
#include <pathfinder/node_client.hpp>

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pathfinder {

/**
 * HTTP JSON-RPC + WebSocket front end.
 *
 * path_find / ripple_path_find are answered from Engine (local memory).
 * Every other command is proxied to the upstream xrpld so the wire
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

}  // namespace pathfinder
