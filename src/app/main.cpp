#include <edgy/config.hpp>
#include <edgy/engine.hpp>
#include <edgy/node_client.hpp>
#include <edgy/server.hpp>

#include <xrpl/basics/Log.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace {

class TeeBuf : public std::streambuf
{
public:
    TeeBuf(std::streambuf* first, std::streambuf* second) : first_(first), second_(second)
    {
    }

protected:
    int
    overflow(int c) override
    {
        if (c == EOF)
            return !EOF;
        auto const ch = static_cast<char>(c);
        if (first_ && first_->sputc(ch) == EOF)
            return EOF;
        if (second_ && second_->sputc(ch) == EOF)
            return EOF;
        return c;
    }

    int
    sync() override
    {
        int r = 0;
        if (first_ && first_->pubsync() != 0)
            r = -1;
        if (second_ && second_->pubsync() != 0)
            r = -1;
        return r;
    }

private:
    std::streambuf* first_;
    std::streambuf* second_;
};

std::string
upstreamServerState(json::Value const& result)
{
    if (result.isMember("info") && result["info"].isObject() &&
        result["info"].isMember("server_state") && result["info"]["server_state"].isString())
        return result["info"]["server_state"].asString();
    if (result.isMember("server_state") && result["server_state"].isString())
        return result["server_state"].asString();
    return {};
}

bool
upstreamStateOk(std::string const& state)
{
    return state == "full" || state == "proposing" || state == "unknown" ||
        state.empty();
}

class CerrTee
{
public:
    explicit CerrTee(std::string const& path)
    {
        if (path.empty())
            return;
        file_.open(path, std::ios::out | std::ios::app);
        if (!file_)
            throw std::runtime_error("cannot open debug log: " + path);
        prev_ = std::cerr.rdbuf();
        tee_.emplace(prev_, file_.rdbuf());
        std::cerr.rdbuf(&*tee_);
    }

    ~CerrTee()
    {
        if (prev_)
            std::cerr.rdbuf(prev_);
    }

    CerrTee(CerrTee const&) = delete;
    CerrTee&
    operator=(CerrTee const&) = delete;

private:
    std::ofstream file_;
    std::streambuf* prev_{nullptr};
    std::optional<TeeBuf> tee_;
};

}  // namespace

int
main(int argc, char** argv)
{
    try
    {
        auto cfg = edgy::Config::fromArgs(argc, argv);
        CerrTee debugTee(cfg.debugLog);

        boost::asio::io_context io{std::max(1, cfg.netThreads)};
        auto work = boost::asio::make_work_guard(io);

        auto node = std::make_shared<edgy::NodeClient>(io, cfg.nodeWs);
        node->run();

        std::vector<std::thread> threads;
        for (int i = 0; i < std::max(1, cfg.netThreads); ++i)
            threads.emplace_back([&io] { io.run(); });

        auto shutdownIo = [&] {
            node->stop();
            work.reset();
            io.stop();
            for (auto& t : threads)
            {
                if (t.joinable())
                    t.join();
            }
        };

        {
            auto const deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds{30};
            while (!node->connected())
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    shutdownIo();
                    throw std::runtime_error("cannot connect to " + cfg.nodeWs);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
        }

        std::cerr << "checking upstream " << cfg.nodeWs << " server_state\n"
                  << std::flush;
        json::Value info;
        try
        {
            info = node->request("server_info", {}, std::chrono::seconds{15});
        }
        catch (std::exception const& ex)
        {
            std::cerr << "upstream " << cfg.nodeWs
                      << " server_state=unknown (server_info failed: " << ex.what()
                      << ")\n"
                      << std::flush;
            shutdownIo();
            throw;
        }
        auto const state = upstreamServerState(info);
        auto const stateLabel = state.empty() ? std::string("unknown") : state;
        std::cerr << "upstream " << cfg.nodeWs << " server_state=" << stateLabel
                  << '\n'
                  << std::flush;
        if (!upstreamStateOk(state))
        {
            shutdownIo();
            throw std::runtime_error(
                "upstream " + cfg.nodeWs + " server_state=" + stateLabel +
                " (need full, proposing, or unknown)");
        }

        edgy::Engine engine(io, cfg, node);
        engine.start();

        edgy::Server server(io, cfg, engine, node);
        server.start();

        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) {
            std::cerr << "stopping\n";
            server.stop();
            engine.stop();
            node->stop();
            work.reset();
            io.stop();
        });

        std::cerr << "Edgy local path_find sidecar\n";
        if (!cfg.configPath.empty())
            std::cerr << "  config    " << cfg.configPath << '\n';
        std::cerr << "  upstream  " << cfg.nodeWs << '\n'
                  << "  listen-ws " << cfg.listenWs << '\n'
                  << "  listen-rpc " << cfg.listenRpc << '\n'
                  << "  workers   " << cfg.workers << '\n'
                  << "  net       " << cfg.netThreads << " event-loop threads\n"
                  << "  update    " << cfg.midCloseDelay.count() << "ms\n"
                  << "  search    " << cfg.search
                  << (cfg.search == edgy::Config::kSearchFull ? " (full)" : "")
                  << '\n'
                  << "  search-fast " << cfg.searchFast
                  << (cfg.searchFast == edgy::Config::kSearchFull ? " (full)" : "")
                  << '\n'
                  << "  timeout   "
                  << (cfg.searchTimeout.count() == 0
                          ? std::string("none")
                          : std::to_string(cfg.searchTimeout.count()) + "ms")
                  << '\n'
                  << "  snapshot  " << (cfg.fullSnapshot ? "full" : "books") << '\n';
        if (!cfg.debugLog.empty())
            std::cerr << "  debug     " << cfg.debugLog << '\n';
        std::cerr << "waiting for snapshot (path_info.server_state = syncing until ready)\n";

        for (auto& t : threads)
            t.join();
        return 0;
    }
    catch (std::exception const& ex)
    {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }
}
