# Edgy

A local `path_find` / `ripple_path_find` sidecar for the **[XRP Ledger](https://github.com/XRPLF/rippled)** (`xrpld`) and **[Xahau](https://github.com/Xahau/xahaud)** (`xahaud`). It full-syncs the validated ledger from the node, keeps that state in memory, and answers path-finding from the local snapshot so searches do not wait on another node.

One CMake build produces **two binaries**. Each links its own `libxrpl`; they cannot share a process.

| Binary | Library | Node | Example config |
| --- | --- | --- | --- |
| `edgy-xrpld` | rippled `libxrpl` | `xrpld` | [`cfg/edgy-xrpl.example.cfg`](cfg/edgy-xrpl.example.cfg) |
| `edgy-xahaud` | xahaud `libxrpl` | `xahaud` | [`cfg/edgy-xahau.example.cfg`](cfg/edgy-xahau.example.cfg) |

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

Current develop builds `edgy-xrpld` and `edgy-xahaud`. Tags through **v0.1.4** shipped a single `edgy-<version>-<os>-<arch>` binary with a `[network]` switch — do not mix that release with these example configs.

Other platforms should [build from source](BUILD.md) until matching assets are attached.

```bash
# after a two-binary release is published, pick both asset names from the page
VER=0.1.5
curl -L -o edgy-xrpld \
  "https://github.com/shortthefomo/edgy/releases/download/v${VER}/edgy-xrpld-${VER}-darwin-arm64"
curl -L -o edgy-xahaud \
  "https://github.com/shortthefomo/edgy/releases/download/v${VER}/edgy-xahaud-${VER}-darwin-arm64"
chmod +x edgy-xrpld edgy-xahaud
./edgy-xrpld --version
# edgy 0.1.5+xrpld.<git>
./edgy-xahaud --version
# edgy 0.1.5+xahaud.<git>
```

Starter configs from the same tag (or copy from a clone):

```bash
curl -L -o edgy-xrpl.cfg \
  https://raw.githubusercontent.com/shortthefomo/edgy/develop/cfg/edgy-xrpl.example.cfg
curl -L -o edgy-xahau.cfg \
  https://raw.githubusercontent.com/shortthefomo/edgy/develop/cfg/edgy-xahau.example.cfg
```

Edit `[node]` in each file to the matching node WebSocket. A full XRPL mainnet snapshot is ~19 million objects and needs tens of gigabytes of RAM. Xahau is smaller. Wait for `snapshot ready` on stderr (or `path_info.info.server_state = full`) before sending `path_find`.

## Build

See [`BUILD.md`](BUILD.md). After a local Release build the binaries are `.build/edgy-xrpld` and `.build/edgy-xahaud`.

## Run

Configuration is an xrpld-style `.cfg` (stanzas). There is one example per binary.

```bash
cp cfg/edgy-xrpl.example.cfg edgy-xrpl.cfg
cp cfg/edgy-xahau.example.cfg edgy-xahau.cfg
# edit [node] in each file
.build/edgy-xrpld --conf edgy-xrpl.cfg
.build/edgy-xahaud --conf edgy-xahau.cfg
```

With no `--conf`, `edgy-xrpld` loads `edgy-xrpl.cfg` (then `edgy.cfg` / `pathfinder.cfg`). `edgy-xahaud` loads `edgy-xahau.cfg` first. Command-line flags override the file.

The examples use different listen ports so both can run on one host:

| | `edgy-xrpld` | `edgy-xahaud` |
| --- | --- | --- |
| Example | [`cfg/edgy-xrpl.example.cfg`](cfg/edgy-xrpl.example.cfg) | [`cfg/edgy-xahau.example.cfg`](cfg/edgy-xahau.example.cfg) |
| Copy to | `edgy-xrpl.cfg` | `edgy-xahau.cfg` |
| `[debug]` | `/tmp/edgy-xrpl.log` | `/tmp/edgy-xahau.log` |
| `[listen-ws]` | `0.0.0.0:6008` | `0.0.0.0:6018` |
| `[listen-rpc]` | `0.0.0.0:5008` | `0.0.0.0:5018` |
| `[node]` | `ws://127.0.0.1:6006` | `ws://127.0.0.1:6006` |
| Native | XRP | XAH |

```
[debug]
/tmp/edgy-xrpl.log

[listen-ws]
0.0.0.0:6008

[listen-rpc]
0.0.0.0:5008

[node]
ws://127.0.0.1:6006

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

`edgy-xahaud` is the same stanzas with the Xahau listen ports and `/tmp/edgy-xahau.log`. There is no `[network]` flag — pick the binary.

Startup connects to `[node]` and requires `server_info.info.server_state` to be `full`, `proposing`, or `unknown`. Anything else (`syncing`, `connected`, …) exits immediately with a fatal error — it will not listen or snapshot.

Point XRPL clients at `ws://127.0.0.1:6008` or `http://127.0.0.1:5008`. Point Xahau clients at `ws://127.0.0.1:6018` or `http://127.0.0.1:5018` if you kept the example ports. Wait for `snapshot ready` on stderr (or `path_info.info.server_state = full`) before expecting path results. `server_info` is forwarded to the upstream node. After each close, stderr should show `txs=N/N … inline with node`.

On Xahau, public API is the same (`ledger`, `ledger_data`, `subscribe`, `server_info`). Native amounts accept and return `XAH` instead of `XRP`. Drop strings (`"1000000"`) work on both networks. Hook / URIToken / HookState objects are kept as blobs or skipped on apply; Offers, lines, accounts, and directories still update. Deletes of unknown types still erase by `LedgerIndex`.

| Flag / stanza | Meaning |
| --- | --- |
| `--conf` / file path | Config file |
| `[node]` / `--node` | Upstream node WebSocket |
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

`EDGY_NODE` overrides `[node]` unless `--node` is also passed. `PATHFINDER_NODE` is still accepted.

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
cfg/edgy-xrpl.example.cfg
cfg/edgy-xahau.example.cfg
include/edgy/           sidecar (ledger, graph, engine, WS/HTTP)
include/edgy/xahau_compat/   headers that adapt xahaud libxrpl
include/xrpld/rpc/      AssetCache / Pathfinder headers (include-path shim)
src/edgy/               sidecar implementation
src/vendor/path/        AssetCache, TrustLine, AccountAssets (Pathfinder on xrpld only)
src/app/main.cpp
tests/edgy_tests.cpp    linked to edgy-xrpld
```

Live search is `FastPathFinder` in `graph.cpp` + `LocalOrderBooks`. RippleCalc comes from rippled `libxrpl` in `edgy-xrpld`, and from xahaud’s payment-engine sources plus xahaud `libxrpl` in `edgy-xahaud`.
