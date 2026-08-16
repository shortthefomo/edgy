# Edgy

A local `path_find` / `ripple_path_find` sidecar for the **[XRP Ledger](https://github.com/XRPLF/rippled)** (`xrpld`) and **[Xahau](https://github.com/Xahau/xahaud)** (`xahaud`). It full-syncs the validated ledger from the node, keeps that state in memory, and answers path-finding from the local snapshot so searches do not wait on another node.

Wire JSON matches the node: `alternatives`, `source_amount`, `paths_computed`, `paths_canonical`, `full_reply`, ledger identity, and the path warning tokens. On Xahau the native asset is **XAH** (drop strings still work).

A Payment can carry at most **6 paths**, each at most **8 hops**. Edgy never returns more than that.

## Why this exists

`xrpld` / `xahaud` Pathfinder is expensive under many concurrent WebSocket sessions. This process:

1. Full-syncs every ledger object once (`ledger_data`, binary).
2. Applies each validated transaction’s `AffectedNodes` locally.
3. Finds book/AMM routes on an in-memory payment graph, then runs RippleCalc only on the best few.
4. Serves a worker pool (default 128, cap 256) so ~100 concurrent `path_find` sockets stay live.
5. Proxies every other RPC/WS command to the upstream node.

## How search works

Not xrpld’s Pathfinder table (level 1–10). Search is a local book graph:

1. **Scan** every 1-hop and 2-hop book pair (set intersection on the adjacency list).
2. **Score** each pair from stored book-tip quality (`ExchangeRate` on directory roots). No offer walk.
3. **RippleCalc** only the best ~8–12 candidates; keep **6** unique hop lists.
4. Longer hops (via the native asset, then up to 8) fill leftover slots as a live WebSocket ages.

HTTP `ripple_path_find` is one mid-depth shot. WebSocket `path_find` starts shallow (fast first reply) and deepens while the socket stays open (about 4s / 12s / 25s / 50s, staggered per session).

Open subscriptions are **repriced every 100ms**. Ledger close also reprices. At most four sessions deepen on a given tick so 100 sockets do not convoy the worker pool. One update is in flight per session.

## Download a release

Binaries and notes: [github.com/shortthefomo/edgy/releases](https://github.com/shortthefomo/edgy/releases). Latest: [releases/latest](https://github.com/shortthefomo/edgy/releases/latest).

Assets are named `edgy-<version>-<os>-<arch>` (for example `edgy-0.1.4-darwin-arm64` on macOS Apple Silicon). Other platforms should [build from source](BUILD.md) until a matching asset is attached.

```bash
# pick the asset name from the latest release page
VER=0.1.4
curl -L -o edgy \
  "https://github.com/shortthefomo/edgy/releases/download/v${VER}/edgy-${VER}-darwin-arm64"
chmod +x edgy
./edgy --version
# edgy 0.1.4+<git>
```

Grab a starter config from the same tag (or copy `cfg/edgy.example.cfg` from a clone):

```bash
curl -L -o edgy.cfg \
  https://raw.githubusercontent.com/shortthefomo/edgy/v0.1.4/cfg/edgy.example.cfg
```

Edit `[node]` to your node’s WebSocket. For Xahau set `[network] xahau` or pass `--xahau`. Then:

```bash
./edgy --conf edgy.cfg
```

A full XRPL mainnet snapshot is ~19 million objects and needs tens of gigabytes of RAM. Xahau is smaller. Wait for `snapshot ready` on stderr (or `path_info.info.server_state = full`) before sending `path_find`.

## Build

See [`BUILD.md`](BUILD.md). After a local Release build the binary is `.build/edgy`.

## Run

Configuration is an xrpld-style `.cfg` (stanzas). Copy the example and edit:

```bash
cp cfg/edgy.example.cfg edgy.cfg
# or, with a downloaded binary: ./edgy --conf edgy.cfg
.build/edgy --conf edgy.cfg
```

If `edgy.cfg` or `cfg/edgy.cfg` exists in the working directory, it is loaded automatically. (`pathfinder.cfg` is still accepted.) Command-line flags override the file.

```
[debug]
/tmp/edgy.log

[listen-ws]
0.0.0.0:6008

[listen-rpc]
0.0.0.0:5008

[node]
ws://127.0.0.1:6006

[network]
xrpl

[workers]
64

[net-threads]
4

[update-ms]
100

[proxy]
1

[search]
full

[search-fast]
full

[timeout-ms]
0

[full-snapshot]
full

[max_total_lines]
1000000

[max_lines_per_account]
50000

[line_chunk_size]
64

[cache_reuse_ledgers]
6
```

```bash
# XRPL (default)
.build/edgy --conf edgy.cfg --workers 64

# Xahau — same binary, switch the ledger family
.build/edgy --conf edgy.cfg --xahau --node ws://127.0.0.1:6006
```

### Xahau

Edgy still links `libxrpl` from rippled. Point `[node]` at an [xahaud](https://github.com/Xahau/xahaud) WebSocket and set the family:

```
[node]
ws://127.0.0.1:6006

[network]
xahau
```

`--xahau` / `--network xahau` and `EDGY_NETWORK=xahau` do the same. Public API is the same (`ledger`, `ledger_data`, `subscribe`, `server_info`). Native amounts accept and return `XAH` instead of `XRP`. Drop strings (`"1000000"`) work on both networks.

Edgy always links rippled’s `libxrpl` (xahaud’s library is `namespace ripple` and a different API — it cannot be swapped in with a config flag, and both cannot live in one binary). Hook / URIToken / HookState objects are kept as blobs or skipped on apply; Offers, lines, accounts, and directories still update. Deletes of unknown types still erase by `LedgerIndex`.

Startup connects to `[node]` and requires `server_info.info.server_state` to be `full`, `proposing`, or `unknown`. Anything else (`syncing`, `connected`, …) exits immediately with a fatal error — it will not listen or snapshot.

Point clients at `ws://127.0.0.1:6008` or `http://127.0.0.1:5008`. Wait for `snapshot ready` on stderr (or `path_info.info.server_state = full`) before expecting path results. `server_info` is forwarded to the upstream node. After each close, stderr should show `txs=N/N … inline with node`.

| Flag / stanza | Meaning |
| --- | --- |
| `--conf` / file path | Config file |
| `[node]` / `--node` | Upstream `xrpld` or `xahaud` WebSocket |
| `[network]` / `--network` | `xrpl` (default) or `xahau`. `--xahau` is the same as `--network xahau` |
| `[listen-ws]` / `--listen-ws` | Local WebSocket (`path_find` + proxy) |
| `[listen-rpc]` / `--listen-rpc` | Local HTTP JSON-RPC |
| `[workers]` / `--workers` | Concurrent search threads (1–256, default 128) |
| `[net-threads]` | Network event-loop threads (default 4). Not disk. |
| `[update-ms]` | Open-socket reprice interval (default 100) |
| `[proxy]` / `--no-proxy` | Forward unknown commands (default 1) |
| `[debug]` / `--debug` | Append diagnostics to this file (`[debug_logfile]` also accepted) |
| `[search]` / `--search` | Target / one-shot depth: `full` (default), `fast`, `mid`, or 0–4 |
| `[search-fast]` / `--search-fast` | First WebSocket reply depth (default `full`) |
| `[timeout-ms]` / `--timeout-ms` | Abort one search after N ms (`0` / `full` = none) |
| `[full-snapshot]` / `--full-snapshot` | `full` (default) loads every object; `0` / `books` loads path types only |
| `[max_total_lines]` | Trust-line cache cap |
| `[max_lines_per_account]` | Per-account line cap |
| `[line_chunk_size]` | Line-fetch chunk |
| `[cache_reuse_ledgers]` | Reuse line cache across this many ledgers |

`EDGY_NODE` overrides `[node]` unless `--node` is also passed. `PATHFINDER_NODE` is still accepted. `EDGY_NETWORK` overrides `[network]` unless `--network` / `--xahau` is also passed.

Default is a full ledger sync and a full book-graph search. Set `[search-fast]` below `[search]` to start WebSocket replies shallow and deepen while the socket stays open.

## API (same as xrpld / xahaud)

**WebSocket `path_find`**

```json
{
  "id": 1,
  "command": "path_find",
  "subcommand": "create",
  "source_account": "r...",
  "destination_account": "r...",
  "destination_amount": { "currency": "USD", "issuer": "r...", "value": "-1" },
  "send_max": "1000000"
}
```

Create returns a first (shallow) result. Later unsolicited frames have `"type": "path_find"` about every 100ms (reprice) and after each closed ledger. Hop width increases while the socket stays open. `close` / `status` work as on the node.

On Xahau, native amounts may use `"currency": "XAH"` or a drop string (`"1000000"`). Replies use `XAH` for native.

**HTTP `ripple_path_find`**

```json
{
  "method": "ripple_path_find",
  "params": [{
    "source_account": "r...",
    "destination_account": "r...",
    "destination_amount": "1000000"
  }]
}
```

One-shot result: `alternatives[]` with `source_amount`, `paths_computed` (≤6), and `paths_canonical: []`.

Optional warning tokens match the node: `path_lines_partial`, `path_revalidate_failed`, `path_source_currencies_truncated`, `path_lines_budget`.

**`path_counts`** (local Edgy counters; `source` is `"edgy"`). **`get_counts`** is forwarded to `[node]`.

```json
{ "command": "path_counts" }
```

```json
{
  "server_state": "full",
  "uptime": "16 minutes, 4 seconds",
  "sessions": 12,
  "inflight": 2,
  "workers_pending": 3,
  "apply_queue": 0,
  "books": 18420,
  "searches": 1402,
  "creates": 80,
  "one_shots": 20,
  "updates": 200,
  "revalidates": 1100,
  "deepens": 22,
  "search_ms_last": 72,
  "search_ms_avg": 81,
  "search_ms_max": 247,
  "pathfind_cache_hits": 900,
  "pathfind_cache_misses": 40,
  "pathfind_cache_lines": 12000,
  "pathfind_lines_loaded": 12000,
  "pathfind_cache_rebuilds": 8,
  "PathRequest": 12,
  "PathFindTrustLine": 12000,
  "objects": 19300000,
  "overlay": 420
}
```

Poll this on a timer to graph load. Fields sit at the top of `result`, matching xrpld’s pathfind cache keys.

## In-memory state

| Piece | Role |
| --- | --- |
| Snapshot hash map | All objects as blobs; O(1) `read` |
| Sorted key vector | `succ` for BookTip (built once at freeze) |
| Overlay map | Ledger-close / tx mutations; not a 19M clone |
| Decoded hot SLEs | Offers, book dirs, AMMs kept after first parse |
| Book graph | Adjacency + reverse index + tip quality per book |

`path_info` (local) includes `objects`, `overlay`, `workers`, `update_ms` (100), and search counters. `server_info` / `server_state` are proxied to `[node]`.

## Layout

```
include/edgy/           sidecar (ledger, graph, engine, WS/HTTP)
include/xrpld/rpc/      AssetCache / Pathfinder headers (include-path shim)
src/edgy/               sidecar implementation
src/vendor/path/        AssetCache, TrustLine, AccountAssets (and unused Pathfinder copy)
src/app/main.cpp
tests/edgy_tests.cpp
```

Live search is `FastPathFinder` in `graph.cpp` + `LocalOrderBooks`. RippleCalc still comes from `libxrpl`.
