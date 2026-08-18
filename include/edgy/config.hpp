#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace edgy {

// Fixed by which binary was built. Not a config/CLI switch.
enum class NetworkKind
{
    xrpl,
    xahau,
};

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
    // Binary ledger_data page size. xrpld/xahaud cap non-admin at 2048.
    static constexpr int kSnapshotPageMax = 2048;
    int snapshotPage{kSnapshotPageMax};

#ifdef EDGY_XAHAU
    static constexpr NetworkKind network = NetworkKind::xahau;
#else
    static constexpr NetworkKind network = NetworkKind::xrpl;
#endif

    [[nodiscard]] bool
    xahau() const
    {
        return network == NetworkKind::xahau;
    }

    [[nodiscard]] char const*
    networkName() const
    {
        return xahau() ? "xahau" : "xrpl";
    }

    [[nodiscard]] char const*
    nativeCurrency() const
    {
        return xahau() ? "XAH" : "XRP";
    }

    [[nodiscard]] char const*
    nodeSoftware() const
    {
        return xahau() ? "xahaud" : "xrpld";
    }

    static Config
    fromFile(std::string const& path);

    static Config
    fromArgs(int argc, char** argv);
};

// If `path` exists and is a non-empty file, rename it to
// `path.YYYYMMDD-HHMMSS` so the next open is a fresh log. Returns the
// backup path, or empty when nothing was moved.
[[nodiscard]] std::string
rotateDebugLog(std::string const& path);

}  // namespace edgy
