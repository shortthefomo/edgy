#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace edgy {

struct Config
{
    std::string nodeWs{"ws://127.0.0.1:6006"};
    std::string listenWs{"0.0.0.0:6008"};
    std::string listenRpc{"0.0.0.0:5008"};
    std::string debugLog;
    std::string configPath;

    // Network event-loop threads (WS / HTTP / upstream). Not disk I/O.
    int netThreads{4};
    int workers{128};

    std::chrono::milliseconds midCloseDelay{100};

    // FastPathFinder depth 0–4. "full" in .cfg is 4 (8-hop scan).
    static constexpr int kSearchFull = 4;
    int search{kSearchFull};
    int searchFast{kSearchFull};
    std::chrono::milliseconds searchTimeout{0};

    std::size_t maxTotalLines{1'000'000};
    std::size_t maxLinesPerAccount{50'000};
    std::size_t lineChunkSize{64};
    std::uint32_t cacheReuseLedgers{6};

    bool proxyOther{true};
    bool fullSnapshot{true};

    static Config
    fromFile(std::string const& path);

    static Config
    fromArgs(int argc, char** argv);
};

}  // namespace edgy
