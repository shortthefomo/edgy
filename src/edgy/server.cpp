#include <edgy/server.hpp>

#include <xrpl/json/json_reader.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/jss.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace edgy {
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

std::pair<std::string, std::string>
splitHostPort(std::string const& ep, std::string const& fallbackPort)
{
    auto const pos = ep.rfind(':');
    if (pos == std::string::npos)
        return {ep, fallbackPort};
    return {ep.substr(0, pos), ep.substr(pos + 1)};
}

json::Value
wrapResponse(json::Value result, json::Value const& id)
{
    bool const err = result.isMember(xrpl::jss::error);
    json::Value out{json::ValueType::Object};
    if (!id.isNull())
        out[xrpl::jss::id] = id;
    out[xrpl::jss::status] = err ? "error" : "success";
    out[xrpl::jss::type] = "response";
    if (!result.isMember(xrpl::jss::status))
        result[xrpl::jss::status] = err ? "error" : "success";
    out[xrpl::jss::result] = std::move(result);
    return out;
}

std::string
lowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string
commandField(json::Value const& v)
{
    if (v.isMember(xrpl::jss::command) && v[xrpl::jss::command].isString())
        return lowerCopy(v[xrpl::jss::command].asString());
    if (v.isMember(xrpl::jss::method) && v[xrpl::jss::method].isString())
        return lowerCopy(v[xrpl::jss::method].asString());
    return {};
}

std::string
commandOf(json::Value const& req)
{
    auto cmd = commandField(req);
    if (!cmd.empty())
        return cmd;
    if (req.isMember(xrpl::jss::params) && req[xrpl::jss::params].isArray() &&
        req[xrpl::jss::params].size() > 0 && req[xrpl::jss::params][0u].isObject())
        return commandField(req[xrpl::jss::params][0u]);
    return {};
}

bool
isPathCommand(std::string const& cmd)
{
    return cmd == "path_find" || cmd == "ripple_path_find";
}

json::Value
paramsOf(json::Value const& req)
{
    if (req.isMember(xrpl::jss::params) && req[xrpl::jss::params].isArray() &&
        req[xrpl::jss::params].size() > 0)
        return req[xrpl::jss::params][0u];
    json::Value p = req;
    p.removeMember(xrpl::jss::id);
    p.removeMember(xrpl::jss::command);
    p.removeMember(xrpl::jss::method);
    p.removeMember(xrpl::jss::jsonrpc);
    p.removeMember(xrpl::jss::ripplerpc);
    return p;
}

}  // namespace

class Server::Impl : public std::enable_shared_from_this<Impl>
{
public:
    Impl(net::io_context& io, Config cfg, Engine& engine, std::shared_ptr<NodeClient> node)
        : io_(io), cfg_(std::move(cfg)), engine_(engine), node_(std::move(node))
    {
        engine_.setLedgerClosedHandler([this](json::Value msg) { broadcastLedger(std::move(msg)); });
    }

    void
    start()
    {
        auto [rpcHost, rpcPort] = splitHostPort(cfg_.listenRpc, "5008");
        auto [wsHost, wsPort] = splitHostPort(cfg_.listenWs, "6008");
        startHttp(rpcHost, rpcPort);
        startWs(wsHost, wsPort);
    }

    void
    stop()
    {
        beast::error_code ec;
        if (httpAcceptor_)
            httpAcceptor_->close(ec);
        if (wsAcceptor_)
            wsAcceptor_->close(ec);
    }

    void
    dispatch(
        json::Value const& req,
        int connId,
        Engine::PushFn push,
        std::function<void(json::Value)> done)
    {
        auto const cmd = commandOf(req);
        auto params = paramsOf(req);
        if (cmd.empty())
        {
            done(xrpl::rpcError(xrpl::RpcInvalidParams));
            return;
        }

        if (cmd == "ripple_path_find")
        {
            engine_.ripplePathFind(std::move(params), std::move(done));
            return;
        }
        if (cmd == "path_find")
        {
            if (!push)
            {
                auto err = xrpl::rpcError(xrpl::RpcInvalidParams);
                err[xrpl::jss::error_message] =
                    "path_find is WebSocket only; use ripple_path_find";
                done(std::move(err));
                return;
            }
            engine_.pathFind(std::move(params), connId, std::move(push), std::move(done));
            return;
        }
        if (cmd == "path_info")
        {
            auto s = engine_.statusJson();
            json::Value info{json::ValueType::Object};
            info["info"] = s;
            info["info"]["server_state"] = engine_.ready() ? "full" : "syncing";
            info["info"]["build_version"] = "edgy";
            done(std::move(info));
            return;
        }
        if (cmd == "path_counts")
        {
            auto counts = engine_.pathCountsJson();
            decorateProxyCounts(counts);
            done(std::move(counts));
            return;
        }
        if (cmd == "ping")
        {
            done(json::Value{json::ValueType::Object});
            return;
        }
        if (cmd == "subscribe" || cmd == "unsubscribe")
        {
            // Never proxy subscribe/unsubscribe on the upstream sync socket.
            bool ledger = false;
            if (params.isMember(xrpl::jss::streams) && params[xrpl::jss::streams].isArray())
            {
                for (auto const& s : params[xrpl::jss::streams])
                {
                    if (s.isString() && s.asString() == "ledger")
                        ledger = true;
                }
            }
            if (ledger)
            {
                std::lock_guard lock(ledgerSubMutex_);
                if (cmd == "subscribe")
                    ledgerSubs_[connId] = push;
                else
                    ledgerSubs_.erase(connId);
            }
            json::Value ack{json::ValueType::Object};
            done(ack);
            if (cmd == "subscribe" && ledger && push)
                push(engine_.ledgerClosedJson());
            return;
        }
        if (cmd == "ledger_closed")
        {
            auto s = engine_.statusJson();
            json::Value r{json::ValueType::Object};
            if (s.isMember(xrpl::jss::ledger_index))
                r[xrpl::jss::ledger_index] = s[xrpl::jss::ledger_index];
            if (s.isMember(xrpl::jss::ledger_hash))
                r[xrpl::jss::ledger_hash] = s[xrpl::jss::ledger_hash];
            done(std::move(r));
            return;
        }

        if (auto const inner = commandField(params); isPathCommand(inner))
        {
            if (inner == "ripple_path_find")
                engine_.ripplePathFind(std::move(params), std::move(done));
            else if (!push)
            {
                auto err = xrpl::rpcError(xrpl::RpcInvalidParams);
                err[xrpl::jss::error_message] =
                    "path_find is WebSocket only; use ripple_path_find";
                done(std::move(err));
            }
            else
                engine_.pathFind(std::move(params), connId, std::move(push), std::move(done));
            return;
        }

        if (!cfg_.proxyOther)
        {
            done(xrpl::rpcError(xrpl::RpcUnknownCommand));
            return;
        }

        noteProxy(cmd);
        std::thread([node = node_, cmd, params = std::move(params), done = std::move(done)]() {
            try
            {
                done(node->request(cmd, params));
            }
            catch (std::exception const& ex)
            {
                auto err = xrpl::rpcError(xrpl::RpcNoCurrent);
                err[xrpl::jss::error_message] = ex.what();
                done(std::move(err));
            }
        }).detach();
    }

private:
    void
    noteProxy(std::string const& cmd)
    {
        proxied_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(proxyMutex_);
        proxiedLast_ = cmd;
        if (proxiedSeen_.insert(cmd).second)
            std::cerr << "proxy " << cmd << " (not a local path_find)\n";
    }

    void
    decorateProxyCounts(json::Value& j) const
    {
        j["proxied"] = static_cast<double>(proxied_.load(std::memory_order_relaxed));
        std::lock_guard lock(proxyMutex_);
        if (!proxiedLast_.empty())
            j["proxied_last"] = proxiedLast_;
    }

    void
    broadcastLedger(json::Value msg)
    {
        std::vector<Engine::PushFn> subs;
        {
            std::lock_guard lock(ledgerSubMutex_);
            subs.reserve(ledgerSubs_.size());
            for (auto const& [id, push] : ledgerSubs_)
            {
                if (push)
                    subs.push_back(push);
            }
        }
        for (auto const& push : subs)
            push(msg);
    }

    void
    startHttp(std::string const& host, std::string const& port)
    {
        tcp::resolver resolver{io_};
        auto const results = resolver.resolve(host, port);
        httpAcceptor_.emplace(io_);
        httpAcceptor_->open(results.begin()->endpoint().protocol());
        httpAcceptor_->set_option(net::socket_base::reuse_address(true));
        httpAcceptor_->bind(results.begin()->endpoint());
        httpAcceptor_->listen(net::socket_base::max_listen_connections);
        acceptHttp();
        std::cerr << "JSON-RPC listening on " << cfg_.listenRpc << '\n';
    }

    void
    startWs(std::string const& host, std::string const& port)
    {
        tcp::resolver resolver{io_};
        auto const results = resolver.resolve(host, port);
        wsAcceptor_.emplace(io_);
        wsAcceptor_->open(results.begin()->endpoint().protocol());
        wsAcceptor_->set_option(net::socket_base::reuse_address(true));
        wsAcceptor_->bind(results.begin()->endpoint());
        wsAcceptor_->listen(net::socket_base::max_listen_connections);
        acceptWs();
        std::cerr << "WebSocket listening on " << cfg_.listenWs << '\n';
    }

    void
    acceptHttp()
    {
        httpAcceptor_->async_accept(
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                if (!ec)
                    self->handleHttp(std::move(socket));
                if (self->httpAcceptor_->is_open())
                    self->acceptHttp();
            });
    }

    void
    handleHttp(tcp::socket socket)
    {
        auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
        stream->expires_after(std::chrono::seconds{30});
        auto buffer = std::make_shared<beast::flat_buffer>();
        auto req = std::make_shared<http::request<http::string_body>>();
        http::async_read(
            *stream,
            *buffer,
            *req,
            [self = shared_from_this(), stream, buffer, req](beast::error_code ec, std::size_t) {
                if (ec)
                    return;
                json::Value body;
                json::Reader().parse(req->body(), body);
                json::Value id;
                if (body.isMember(xrpl::jss::id))
                    id = body[xrpl::jss::id];
                self->dispatch(body, 0, {}, [io = &self->io_, stream, req, id](json::Value result) {
                    net::post(*io, [stream, req, id, result = std::move(result)]() mutable {
                        auto wrapped = wrapResponse(std::move(result), id);
                        auto res = std::make_shared<http::response<http::string_body>>(
                            http::status::ok, req->version());
                        res->set(http::field::content_type, "application/json");
                        res->keep_alive(req->keep_alive());
                        res->body() = json::to_string(wrapped);
                        res->prepare_payload();
                        stream->expires_after(std::chrono::seconds{30});
                        http::async_write(
                            *stream, *res, [stream, res](beast::error_code, std::size_t) {
                                beast::error_code ignored;
                                stream->socket().shutdown(tcp::socket::shutdown_send, ignored);
                            });
                    });
                });
            });
    }

    void
    acceptWs()
    {
        wsAcceptor_->async_accept(
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                if (!ec)
                    self->handleWs(std::move(socket));
                if (self->wsAcceptor_->is_open())
                    self->acceptWs();
            });
    }

    void
    handleWs(tcp::socket socket)
    {
        auto const connId = nextConn_.fetch_add(1);
        auto strand = net::make_strand(io_);
        net::co_spawn(
            strand,
            [self = shared_from_this(), socket = std::move(socket), connId, strand]() mutable
                -> net::awaitable<void> {
                auto ws = std::make_shared<websocket::stream<tcp::socket>>(std::move(socket));
                websocket::stream_base::timeout to{};
                to.handshake_timeout = std::chrono::seconds{30};
                to.idle_timeout = std::chrono::minutes{5};
                to.keep_alive_pings = true;
                ws->set_option(to);
                ws->read_message_max(16 * 1024 * 1024);
                try
                {
                    co_await ws->async_accept(net::bind_executor(strand, net::use_awaitable));

                    auto queue = std::make_shared<std::deque<std::string>>();
                    auto writing = std::make_shared<bool>(false);
                    auto closed = std::make_shared<std::atomic<bool>>(false);

                    auto fail = [ws, closed] {
                        if (closed->exchange(true))
                            return;
                        beast::error_code ignored;
                        ws->next_layer().cancel(ignored);
                        ws->next_layer().shutdown(tcp::socket::shutdown_both, ignored);
                    };

                    auto pump = std::make_shared<std::function<void()>>();
                    *pump = [ws, queue, writing, closed, pump, strand, fail]() {
                        if (closed->load(std::memory_order_acquire))
                            return;
                        if (queue->empty())
                        {
                            *writing = false;
                            return;
                        }
                        auto held = std::make_shared<std::string>(std::move(queue->front()));
                        queue->pop_front();
                        ws->async_write(
                            net::buffer(*held),
                            net::bind_executor(
                                strand,
                                [held, pump, closed, fail](beast::error_code ec, std::size_t) {
                                    if (ec)
                                    {
                                        fail();
                                        return;
                                    }
                                    (*pump)();
                                }));
                    };

                    auto send = [strand, queue, writing, pump, closed, fail](std::string text) {
                        net::post(strand, [text = std::move(text),
                                           queue,
                                           writing,
                                           pump,
                                           closed,
                                           fail]() mutable {
                            if (closed->load(std::memory_order_acquire))
                                return;
                            if (queue->size() >= 64)
                            {
                                fail();
                                return;
                            }
                            queue->push_back(std::move(text));
                            if (!*writing)
                            {
                                *writing = true;
                                (*pump)();
                            }
                        });
                    };

                    Engine::PushFn push = [send](json::Value msg) {
                        send(json::to_string(msg));
                    };

                    for (;;)
                    {
                        if (closed->load(std::memory_order_acquire))
                            break;
                        beast::flat_buffer buffer;
                        co_await ws->async_read(
                            buffer, net::bind_executor(strand, net::use_awaitable));
                        json::Value body;
                        if (!json::Reader().parse(beast::buffers_to_string(buffer.data()), body))
                            continue;
                        json::Value id;
                        if (body.isMember(xrpl::jss::id))
                            id = body[xrpl::jss::id];
                        self->dispatch(body, connId, push, [push, id](json::Value result) {
                            push(wrapResponse(std::move(result), id));
                        });
                    }
                }
                catch (...)
                {
                }
                {
                    std::lock_guard lock(self->ledgerSubMutex_);
                    self->ledgerSubs_.erase(connId);
                }
                self->engine_.dropConnection(connId);
                co_return;
            },
            net::detached);
    }

    net::io_context& io_;
    Config cfg_;
    Engine& engine_;
    std::shared_ptr<NodeClient> node_;
    std::optional<tcp::acceptor> httpAcceptor_;
    std::optional<tcp::acceptor> wsAcceptor_;
    std::atomic<int> nextConn_{1};
    std::mutex ledgerSubMutex_;
    std::unordered_map<int, Engine::PushFn> ledgerSubs_;
    std::atomic<std::uint64_t> proxied_{0};
    mutable std::mutex proxyMutex_;
    std::string proxiedLast_;
    std::unordered_set<std::string> proxiedSeen_;
};

Server::Server(
    boost::asio::io_context& io,
    Config cfg,
    Engine& engine,
    std::shared_ptr<NodeClient> node)
    : impl_(std::make_shared<Impl>(io, std::move(cfg), engine, std::move(node)))
{
}

Server::~Server()
{
    if (impl_)
        impl_->stop();
}

void
Server::start()
{
    impl_->start();
}

void
Server::stop()
{
    impl_->stop();
}

}  // namespace edgy
