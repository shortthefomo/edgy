#include <edgy/node_client.hpp>

#include <xrpl/json/json_reader.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/jss.h>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace edgy {
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace {

struct ParsedUrl
{
    bool tls{false};
    std::string host;
    std::string port;
    std::string path{"/"};
};

ParsedUrl
parseWsUrl(std::string url)
{
    ParsedUrl out;
    if (url.starts_with("wss://"))
    {
        out.tls = true;
        url.erase(0, 6);
        out.port = "443";
    }
    else if (url.starts_with("ws://"))
    {
        url.erase(0, 5);
        out.port = "80";
    }
    else
    {
        throw std::runtime_error("node URL must start with ws:// or wss://");
    }

    auto const slash = url.find('/');
    std::string hostport = url.substr(0, slash);
    if (slash != std::string::npos)
        out.path = url.substr(slash);
    auto const colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos)
    {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
    }
    else
    {
        out.host = hostport;
    }
    if (out.host.empty())
        throw std::runtime_error("node URL missing host");
    return out;
}

}  // namespace

class NodeClient::Impl : public std::enable_shared_from_this<Impl>
{
public:
    Impl(net::io_context& io, std::string url)
        : io_(io), strand_(net::make_strand(io)), url_(std::move(url))
    {
    }

    void
    run()
    {
        parsed_ = parseWsUrl(url_);
        net::post(strand_, [self = shared_from_this()] {
            self->closing_ = false;
            self->wantConnect_ = true;
            self->pendingReconnect_ = true;
            self->startConnect();
        });
    }

    void
    stop()
    {
        net::post(strand_, [self = shared_from_this()] {
            self->closing_ = true;
            self->wantConnect_ = false;
            self->pendingReconnect_ = false;
            self->cancelStream();
        });
        failPending("disconnected");
    }

    [[nodiscard]] bool
    connected() const
    {
        return connected_.load();
    }

    json::Value
    request(std::string const& command, json::Value params, std::chrono::milliseconds timeout)
    {
        auto const id = nextId_.fetch_add(1);
        auto promise = std::make_shared<std::promise<json::Value>>();
        auto future = promise->get_future();
        {
            std::lock_guard lock(pendingMutex_);
            pending_[id] = promise;
        }

        json::Value req = params;
        if (!req.isObject())
            req = json::Value{json::ValueType::Object};
        req[xrpl::jss::id] = id;
        req[xrpl::jss::command] = command;

        net::post(strand_, [self = shared_from_this(), body = json::to_string(req)] {
            self->enqueueWrite(std::move(body));
        });

        if (future.wait_for(timeout) != std::future_status::ready)
        {
            std::lock_guard lock(pendingMutex_);
            pending_.erase(id);
            throw std::runtime_error("upstream RPC timeout: " + command);
        }
        return future.get();
    }

    void
    onLedger(StreamHandler h)
    {
        std::lock_guard lock(handlerMutex_);
        onLedger_ = std::move(h);
    }

    void
    onTransaction(StreamHandler h)
    {
        std::lock_guard lock(handlerMutex_);
        onTx_ = std::move(h);
    }

    void
    onDisconnect(std::function<void(std::string const&)> h)
    {
        std::lock_guard lock(handlerMutex_);
        onDisconnect_ = std::move(h);
    }

private:
    void
    cancelStream()
    {
        beast::error_code ec;
        if (ws_)
            ws_->next_layer().cancel(ec);
        if (wss_)
            beast::get_lowest_layer(*wss_).cancel(ec);
    }

    void
    startConnect()
    {
        if (closing_ || !wantConnect_)
            return;
        // Never destroy a Beast stream while async_read/write is outstanding.
        if (reading_ || writing_ || ws_ || wss_)
        {
            pendingReconnect_ = true;
            cancelStream();
            return;
        }
        pendingReconnect_ = false;
        doConnect();
    }

    void
    recycleStream()
    {
        if (reading_ || writing_)
            return;
        ws_.reset();
        wss_.reset();
        writes_.clear();
        writing_ = false;
        buffer_.consume(buffer_.size());
        if (pendingReconnect_ && !closing_ && wantConnect_)
            startConnect();
    }

    void
    streamFailed(std::string const& why)
    {
        connected_.store(false);
        notifyDisconnect(why);
        failPending(why);
        cancelStream();
        recycleStream();
    }

    void
    doConnect()
    {
        try
        {
            tcp::resolver resolver{io_};
            auto const results = resolver.resolve(parsed_.host, parsed_.port);
            if (parsed_.tls)
            {
                ssl_.emplace(ssl::context::tlsv12_client);
                ssl_->set_default_verify_paths();
                wss_.emplace(io_, *ssl_);
                net::connect(beast::get_lowest_layer(*wss_), results);
                if (!SSL_set_tlsext_host_name(wss_->next_layer().native_handle(), parsed_.host.c_str()))
                    throw std::runtime_error("SNI failed");
                wss_->next_layer().handshake(ssl::stream_base::client);
                websocket::stream_base::timeout to{};
                to.handshake_timeout = std::chrono::seconds{30};
                to.idle_timeout = std::chrono::minutes{30};
                to.keep_alive_pings = true;
                wss_->set_option(to);
                wss_->read_message_max(64 * 1024 * 1024);
                wss_->handshake(parsed_.host, parsed_.path);
            }
            else
            {
                ws_.emplace(io_);
                net::connect(ws_->next_layer(), results);
                websocket::stream_base::timeout to{};
                to.handshake_timeout = std::chrono::seconds{30};
                to.idle_timeout = std::chrono::minutes{30};
                to.keep_alive_pings = true;
                ws_->set_option(to);
                ws_->read_message_max(64 * 1024 * 1024);
                ws_->handshake(parsed_.host, parsed_.path);
            }
            connected_.store(true);
            reading_ = true;
            doRead();
        }
        catch (std::exception const& ex)
        {
            connected_.store(false);
            ws_.reset();
            wss_.reset();
            notifyDisconnect(ex.what());
        }
    }

    void
    doRead()
    {
        auto onRead = net::bind_executor(
            strand_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec)
                {
                    self->reading_ = false;
                    self->streamFailed(ec.message());
                    return;
                }
                self->handleFrame();
                self->buffer_.consume(self->buffer_.size());
                self->doRead();
            });
        if (wss_)
            wss_->async_read(buffer_, onRead);
        else if (ws_)
            ws_->async_read(buffer_, onRead);
        else
            reading_ = false;
    }

    void
    handleFrame()
    {
        auto const text = beast::buffers_to_string(buffer_.data());
        json::Value msg;
        if (!json::Reader().parse(text, msg) || !msg.isObject())
            return;

        int replyId = 0;
        bool haveId = false;
        if (msg.isMember(xrpl::jss::id))
        {
            if (msg[xrpl::jss::id].isIntegral())
            {
                replyId = msg[xrpl::jss::id].asInt();
                haveId = true;
            }
            else if (msg[xrpl::jss::id].isString())
            {
                try
                {
                    replyId = std::stoi(msg[xrpl::jss::id].asString());
                    haveId = true;
                }
                catch (...)
                {
                }
            }
        }
        if (haveId)
        {
            auto const id = replyId;
            std::shared_ptr<std::promise<json::Value>> promise;
            {
                std::lock_guard lock(pendingMutex_);
                auto it = pending_.find(id);
                if (it != pending_.end())
                {
                    promise = std::move(it->second);
                    pending_.erase(it);
                }
            }
            if (promise)
            {
                if (msg.isMember(xrpl::jss::result))
                    promise->set_value(msg[xrpl::jss::result]);
                else
                    promise->set_value(msg);
                return;
            }
        }

        std::string type;
        if (msg.isMember(xrpl::jss::type) && msg[xrpl::jss::type].isString())
            type = msg[xrpl::jss::type].asString();

        StreamHandler ledger;
        StreamHandler tx;
        {
            std::lock_guard lock(handlerMutex_);
            ledger = onLedger_;
            tx = onTx_;
        }
        if (type == "ledgerClosed" && ledger)
            ledger(msg);
        else if (type == "transaction" && tx)
            tx(msg);
    }

    void
    enqueueWrite(std::string body)
    {
        if (closing_ || (!ws_ && !wss_))
            return;
        writes_.push_back(std::move(body));
        if (!writing_)
        {
            writing_ = true;
            doWrite();
        }
    }

    void
    doWrite()
    {
        if (writes_.empty())
        {
            writing_ = false;
            recycleStream();
            return;
        }
        auto cb = net::bind_executor(
            strand_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (!self->writes_.empty())
                    self->writes_.pop_front();
                if (ec)
                {
                    self->writing_ = false;
                    self->streamFailed(ec.message());
                    return;
                }
                self->doWrite();
            });
        if (wss_)
            wss_->async_write(net::buffer(writes_.front()), cb);
        else if (ws_)
            ws_->async_write(net::buffer(writes_.front()), cb);
        else
        {
            writing_ = false;
            recycleStream();
        }
    }

    void
    failPending(std::string const& why)
    {
        std::lock_guard lock(pendingMutex_);
        for (auto& [id, p] : pending_)
        {
            try
            {
                p->set_exception(std::make_exception_ptr(std::runtime_error(why)));
            }
            catch (...)
            {
            }
        }
        pending_.clear();
    }

    void
    notifyDisconnect(std::string const& why)
    {
        std::function<void(std::string const&)> h;
        {
            std::lock_guard lock(handlerMutex_);
            h = onDisconnect_;
        }
        if (h)
            h(why);
    }

    net::io_context& io_;
    net::strand<net::io_context::executor_type> strand_;
    std::string url_;
    ParsedUrl parsed_;
    std::optional<ssl::context> ssl_;
    std::optional<websocket::stream<tcp::socket>> ws_;
    std::optional<websocket::stream<beast::ssl_stream<tcp::socket>>> wss_;
    beast::flat_buffer buffer_;
    std::atomic<bool> connected_{false};
    bool closing_{false};
    bool wantConnect_{false};
    bool pendingReconnect_{false};
    bool reading_{false};
    bool writing_{false};
    std::deque<std::string> writes_;
    std::atomic<int> nextId_{1};
    std::mutex pendingMutex_;
    std::unordered_map<int, std::shared_ptr<std::promise<json::Value>>> pending_;
    std::mutex handlerMutex_;
    StreamHandler onLedger_;
    StreamHandler onTx_;
    std::function<void(std::string const&)> onDisconnect_;
};

NodeClient::NodeClient(boost::asio::io_context& io, std::string url)
    : impl_(std::make_shared<Impl>(io, std::move(url)))
{
}

NodeClient::~NodeClient()
{
    if (impl_)
        impl_->stop();
}

void
NodeClient::run()
{
    impl_->run();
}

void
NodeClient::stop()
{
    impl_->stop();
}

bool
NodeClient::connected() const
{
    return impl_->connected();
}

json::Value
NodeClient::request(std::string const& command, json::Value params, std::chrono::milliseconds timeout)
{
    return impl_->request(command, std::move(params), timeout);
}

void
NodeClient::onLedger(StreamHandler handler)
{
    impl_->onLedger(std::move(handler));
}

void
NodeClient::onTransaction(StreamHandler handler)
{
    impl_->onTransaction(std::move(handler));
}

void
NodeClient::onDisconnect(std::function<void(std::string const&)> handler)
{
    impl_->onDisconnect(std::move(handler));
}

}  // namespace edgy
