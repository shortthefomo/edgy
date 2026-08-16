#pragma once

#include <xrpl/json/json_value.h>

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace edgy {

/**
 * Single WebSocket connection to an xrpld or xahaud node.
 *
 * Request/response is correlated by numeric `id`. Unsolicited stream frames
 * (ledger, transaction) are delivered to the registered handlers.
 */
class NodeClient : public std::enable_shared_from_this<NodeClient>
{
public:
    using Json = json::Value;
    using StreamHandler = std::function<void(Json const&)>;

    NodeClient(boost::asio::io_context& io, std::string url);
    ~NodeClient();

    NodeClient(NodeClient const&) = delete;
    NodeClient&
    operator=(NodeClient const&) = delete;

    void
    run();

    void
    stop();

    [[nodiscard]] bool
    connected() const;

    /**
     * Send a JSON-RPC command and wait for the matching response `result`.
     * Throws on timeout, disconnect, or a node-level transport failure.
     */
    Json
    request(
        std::string const& command,
        Json params = Json{json::ValueType::Object},
        std::chrono::milliseconds timeout = std::chrono::seconds{30});

    void
    onLedger(StreamHandler handler);

    void
    onTransaction(StreamHandler handler);

    void
    onDisconnect(std::function<void(std::string const&)> handler);

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace edgy
