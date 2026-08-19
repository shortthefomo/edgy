#include <edgy/config.hpp>
#include <edgy/version.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edgy {
namespace {

void
usage()
{
    std::cerr
        << "Usage: edgy-xrpld|edgy-xahaud [options] [edgy-xrpl.cfg|edgy-xahau.cfg]\n"
        << "  --conf <file>               xrpld-style config (stanzas)\n"
        << "  --node <ws://host:port>     Upstream node WebSocket\n"
        << "  --listen-ws <host:port>     Local WebSocket\n"
        << "  --listen-rpc <host:port>    Local JSON-RPC HTTP\n"
        << "  --workers <n>               Concurrent path_find workers\n"
        << "  --debug <file>              Diagnostic log (rotated on each start)\n"
        << "  --search <fast|mid|full>    Target / one-shot depth (default full)\n"
        << "  --search-fast <fast|mid|full> First WebSocket reply depth (default fast)\n"
        << "  --timeout-ms <n>            Abort one search after N ms (0 = none)\n"
        << "  --full-snapshot [0|1|full]  Full ledger vs books-only (default full)\n"
        << "  --snapshot-page <n>         ledger_data objects per page (default 2048)\n"
        << "  --no-proxy                  Do not forward unknown RPCs to the node\n"
        << "  --version                   Print version and exit\n"
        << "  --help\n"
        << "\n"
        << "File stanzas (value on the following line, like xrpld.cfg):\n"
        << "  [node]  [listen-ws]  [listen-rpc]  [workers]  [net-threads]\n"
        << "  [update-ms]  [proxy]  [debug]\n"
        << "  [search]  [search-fast]  [timeout-ms]  [full-snapshot]\n"
        << "  [snapshot-page]\n"
        << "  [max_total_lines]  [max_lines_per_account]\n"
        << "  [line_chunk_size]  [cache_reuse_ledgers]\n"
        << "  [path_find]  (optional key=value block for the four line-cache keys)\n";
}

std::string
trim(std::string_view s)
{
    auto const isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return std::string{s};
}

std::string
normalizeName(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::replace(s.begin(), s.end(), '_', '-');
    return s;
}

bool
parseFlag(std::string const& v)
{
    auto const s = normalizeName(v);
    if (s == "0" || s == "false" || s == "no" || s == "off")
        return false;
    if (s == "1" || s == "true" || s == "yes" || s == "on")
        return true;
    throw std::runtime_error("invalid flag value: " + v);
}

int
parseSearchLevel(std::string const& v)
{
    auto const s = normalizeName(v);
    if (s == "full")
        return Config::kSearchFull;
    if (s == "mid")
        return Config::kSearchMid;
    if (s == "fast")
        return Config::kSearchFast;
    throw std::runtime_error("search level must be fast, mid, or full");
}

bool
parseSnapshot(std::string const& v)
{
    auto const s = normalizeName(v);
    if (s == "full" || s == "all")
        return true;
    if (s == "books" || s == "partial" || s == "offers")
        return false;
    return parseFlag(v);
}

[[noreturn]] void
rejectedNetworkSwitch(char const* how)
{
    throw std::runtime_error(
        std::string(how) +
        " is gone; run edgy-xrpld against xrpld or edgy-xahaud against xahaud");
}

std::chrono::milliseconds
parseTimeout(std::string const& v)
{
    auto const s = normalizeName(v);
    if (s == "full" || s == "none" || s == "off")
        return std::chrono::milliseconds{0};
    int n = std::stoi(v);
    if (n < 0)
        n = 0;
    return std::chrono::milliseconds{n};
}

void
applyScalar(Config& cfg, std::string const& section, std::string const& value)
{
    if (section == "node")
        cfg.nodeWs = value;
    else if (section == "listen-ws" || section == "port-ws")
        cfg.listenWs = value;
    else if (section == "listen-rpc" || section == "port-rpc")
        cfg.listenRpc = value;
    else if (section == "debug" || section == "debug-logfile")
        cfg.debugLog = value;
    else if (section == "workers")
        cfg.workers = std::stoi(value);
    else if (section == "net-threads")
        cfg.netThreads = std::stoi(value);
    else if (section == "update-ms" || section == "mid-close-ms")
        cfg.midCloseDelay = std::chrono::milliseconds{std::stoi(value)};
    else if (section == "proxy")
        cfg.proxyOther = parseFlag(value);
    else if (section == "search" || section == "path-search" || section == "path-search-max")
        cfg.search = parseSearchLevel(value);
    else if (section == "search-fast" || section == "path-search-fast")
        cfg.searchFast = parseSearchLevel(value);
    else if (section == "timeout-ms")
        cfg.searchTimeout = parseTimeout(value);
    else if (section == "full-snapshot")
        cfg.fullSnapshot = parseSnapshot(value);
    else if (section == "snapshot-page" || section == "snapshot-limit" ||
             section == "ledger-data-limit")
        cfg.snapshotPage = std::stoi(value);
    else if (section == "network" || section == "node-type")
        ;  // leftover from the single-binary switch; family is the executable
    else if (section == "max-total-lines")
        cfg.maxTotalLines = static_cast<std::size_t>(std::stoull(value));
    else if (section == "max-lines-per-account")
        cfg.maxLinesPerAccount = static_cast<std::size_t>(std::stoull(value));
    else if (section == "line-chunk-size")
        cfg.lineChunkSize = static_cast<std::size_t>(std::stoull(value));
    else if (section == "cache-reuse-ledgers")
        cfg.cacheReuseLedgers = static_cast<std::uint32_t>(std::stoul(value));
    else if (section == "path-find")
        throw std::runtime_error("[path_find] uses key=value lines, not a single value");
    else
        throw std::runtime_error("unknown config section [" + section + "]");
}

void
applyKeyed(Config& cfg, std::string const& section, std::string const& key, std::string const& value)
{
    if (section == "path-find")
    {
        if (key == "max-total-lines")
            cfg.maxTotalLines = static_cast<std::size_t>(std::stoull(value));
        else if (key == "max-lines-per-account")
            cfg.maxLinesPerAccount = static_cast<std::size_t>(std::stoull(value));
        else if (key == "line-chunk-size")
            cfg.lineChunkSize = static_cast<std::size_t>(std::stoull(value));
        else if (key == "cache-reuse-ledgers")
            cfg.cacheReuseLedgers = static_cast<std::uint32_t>(std::stoul(value));
        else
            throw std::runtime_error("unknown [path_find] key: " + key);
        return;
    }
    if (key == section || key == "value")
    {
        applyScalar(cfg, section, value);
        return;
    }
    throw std::runtime_error("[" + section + "] does not take key=value (" + key + ")");
}

void
clamp(Config& cfg)
{
    if (cfg.netThreads < 1)
        cfg.netThreads = 1;
    if (cfg.workers < 1)
        cfg.workers = 1;
    if (cfg.workers > 256)
        cfg.workers = 256;
    if (cfg.midCloseDelay.count() < 20)
        cfg.midCloseDelay = std::chrono::milliseconds{20};
    if (cfg.lineChunkSize < 1)
        cfg.lineChunkSize = 1;
    cfg.search = std::clamp(cfg.search, 0, Config::kSearchFull);
    cfg.searchFast = std::clamp(cfg.searchFast, 0, cfg.search);
    if (cfg.searchTimeout.count() < 0)
        cfg.searchTimeout = std::chrono::milliseconds{0};
    if (cfg.snapshotPage < 1)
        cfg.snapshotPage = 1;
    if (cfg.snapshotPage > Config::kSnapshotPageMax)
        cfg.snapshotPage = Config::kSnapshotPageMax;
}

}  // namespace

void
loadConfigFile(Config& cfg, std::string const& path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open config file: " + path);

    cfg.configPath = path;
    std::string line;
    std::string section;
    int lineNo = 0;
    while (std::getline(in, line))
    {
        ++lineNo;
        auto const hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;

        if (line.front() == '[' && line.back() == ']')
        {
            section = normalizeName(line.substr(1, line.size() - 2));
            if (section.empty())
                throw std::runtime_error(path + ":" + std::to_string(lineNo) + ": empty section");
            continue;
        }
        if (section.empty())
            throw std::runtime_error(path + ":" + std::to_string(lineNo) + ": value outside a section");

        try
        {
            auto const eq = line.find('=');
            if (eq != std::string::npos)
            {
                auto key = normalizeName(trim(line.substr(0, eq)));
                auto value = trim(line.substr(eq + 1));
                applyKeyed(cfg, section, key, value);
            }
            else
            {
                applyScalar(cfg, section, line);
            }
        }
        catch (std::exception const& ex)
        {
            throw std::runtime_error(path + ":" + std::to_string(lineNo) + ": " + ex.what());
        }
    }
}

Config
Config::fromFile(std::string const& path)
{
    Config cfg;
    loadConfigFile(cfg, path);
    clamp(cfg);
    return cfg;
}

Config
Config::fromArgs(int argc, char** argv)
{
    Config cfg;
    std::string confPath;
    std::vector<std::pair<std::string_view, std::string>> cli;

    for (int i = 1; i < argc; ++i)
    {
        std::string_view const arg{argv[i]};
        auto need = [&](char const* name) -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h")
        {
            usage();
            std::exit(0);
        }
        else if (arg == "--version" || arg == "-v")
        {
            std::cout << "edgy " << versionString() << '\n';
            std::exit(0);
        }
        else if (arg == "--conf" || arg == "-c")
        {
            confPath = need("--conf");
        }
        else if (arg == "--node")
        {
            cli.emplace_back("node", need("--node"));
        }
        else if (arg == "--network" || arg == "--xahau" || arg == "--xahaud" ||
                 arg == "--xrpl" || arg == "--xrpld")
        {
            rejectedNetworkSwitch(std::string(arg).c_str());
        }
        else if (arg == "--listen-ws")
        {
            cli.emplace_back("listen-ws", need("--listen-ws"));
        }
        else if (arg == "--listen-rpc")
        {
            cli.emplace_back("listen-rpc", need("--listen-rpc"));
        }
        else if (arg == "--workers")
        {
            cli.emplace_back("workers", need("--workers"));
        }
        else if (arg == "--debug")
        {
            cli.emplace_back("debug", need("--debug"));
        }
        else if (arg == "--no-proxy")
        {
            cli.emplace_back("proxy", "0");
        }
        else if (arg == "--search")
        {
            cli.emplace_back("search", need("--search"));
        }
        else if (arg == "--search-fast")
        {
            cli.emplace_back("search-fast", need("--search-fast"));
        }
        else if (arg == "--timeout-ms")
        {
            cli.emplace_back("timeout-ms", need("--timeout-ms"));
        }
        else if (arg == "--full-snapshot")
        {
            if (i + 1 < argc && argv[i + 1][0] != '-')
                cli.emplace_back("full-snapshot", need("--full-snapshot"));
            else
                cli.emplace_back("full-snapshot", "full");
        }
        else if (arg == "--no-full-snapshot")
        {
            cli.emplace_back("full-snapshot", "0");
        }
        else if (arg == "--snapshot-page")
        {
            cli.emplace_back("snapshot-page", need("--snapshot-page"));
        }
        else if (!arg.empty() && arg.front() != '-' &&
                 (arg.ends_with(".cfg") || arg.ends_with(".conf")))
        {
            if (!confPath.empty())
                throw std::runtime_error("multiple config files");
            confPath = std::string{arg};
        }
        else
        {
            throw std::runtime_error("unknown argument: " + std::string(arg));
        }
    }

    if (confPath.empty())
    {
#ifdef EDGY_XAHAU
        static char const* const kCandidates[] = {
            "edgy-xahau.cfg",
            "cfg/edgy-xahau.cfg",
            "edgy.cfg",
            "cfg/edgy.cfg",
            "pathfinder.cfg",
            "cfg/pathfinder.cfg",
        };
#else
        static char const* const kCandidates[] = {
            "edgy-xrpl.cfg",
            "cfg/edgy-xrpl.cfg",
            "edgy.cfg",
            "cfg/edgy.cfg",
            "pathfinder.cfg",
            "cfg/pathfinder.cfg",
        };
#endif
        for (char const* candidate : kCandidates)
        {
            std::ifstream probe(candidate);
            if (probe)
            {
                confPath = candidate;
                break;
            }
        }
    }

    if (!confPath.empty())
        loadConfigFile(cfg, confPath);

    if (auto const* env = std::getenv("EDGY_NODE"))
        cfg.nodeWs = env;
    else if (auto const* env = std::getenv("PATHFINDER_NODE"))
        cfg.nodeWs = env;

    if (std::getenv("EDGY_NETWORK"))
        rejectedNetworkSwitch("EDGY_NETWORK");

    for (auto const& [section, value] : cli)
        applyScalar(cfg, std::string{section}, value);

    clamp(cfg);
    return cfg;
}

std::string
rotateDebugLog(std::string const& path)
{
    if (path.empty())
        return {};

    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) == 0)
        return {};

    auto const now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::ostringstream stamp;
    stamp << std::put_time(&tm, "%Y%m%d-%H%M%S");
    auto backup = path + "." + stamp.str();
    for (int n = 2; fs::exists(backup, ec); ++n)
        backup = path + "." + stamp.str() + "-" + std::to_string(n);

    fs::rename(path, backup, ec);
    if (ec)
        return {};
    return backup;
}

}  // namespace edgy
