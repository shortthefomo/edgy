# PathFinder

A local XRPL `path_find` / `ripple_path_find` sidecar. It full-syncs the validated ledger from an `xrpld` node, keeps that state in memory, and answers path-finding from the local snapshot so searches do not wait on another node.

Wire JSON matches xrpld: `alternatives`, `source_amount`, `paths_computed`, `paths_canonical`, `full_reply`, ledger identity, and the path warning tokens.

A Payment can carry at most **6 paths**, each at most **8 hops**. PathFinder never returns more than that.

## Why this exists

`xrpld` Pathfinder is expensive under many concurrent WebSocket sessions. This process:

1. Full-syncs every ledger object once (`ledger_data`, binary).
2. Applies each validated transaction’s binary `AffectedNodes` locally.
3. Finds book/AMM routes on an in-memory payment graph, then runs RippleCalc only on the best few.
4. Serves a worker pool (default 128, cap 256) so ~100 concurrent `path_find` sockets stay live.
5. Proxies every other RPC/WS command to the upstream node.

Do not edit `~/Dev/Ledgers/rippled`. All work stays in this tree.

## How search works

Not xrpld’s Pathfinder table (level 1–10). Search is a local book graph:

1. **Scan** every 1-hop and 2-hop book pair (set intersection on the adjacency list).
2. **Score** each pair from stored book-tip quality (`ExchangeRate` on directory roots). No offer walk.
3. **RippleCalc** only the best ~8–12 candidates; keep **6** unique hop lists.
4. Longer hops (via XRP, then up to 8) fill leftover slots as a live WebSocket ages.

HTTP `ripple_path_find` is one mid-depth shot. WebSocket `path_find` starts shallow (fast first reply) and deepens while the socket stays open (about 4s / 12s / 25s / 50s, staggered per session).

Open subscriptions are **repriced every 100ms**. Ledger close also reprices. At most four sessions deepen on a given tick so 100 sockets do not convoy the worker pool. One update is in flight per session.

## Build

Needs the sibling rippled tree configured and `libxrpl.a` built (Conan toolchain in `rippled/.build`).

```bash
cmake -S . -B .build \
  -DCMAKE_TOOLCHAIN_FILE=../rippled/.build/build/generators/conan_toolchain.cmake \
  -DRIPPLED_ROOT=../rippled \
  -DRIPPLED_BUILD=../rippled/.build
cmake --build .build -j
ctest --test-dir .build --output-on-failure
```

## Run

Configuration is an xrpld-style `.cfg` (stanzas). Copy the example and edit:

```bash
cp cfg/pathfinder.example.cfg pathfinder.cfg
.build/pathfinder --conf pathfinder.cfg
```

If `pathfinder.cfg` or `cfg/pathfinder.cfg` exists in the working directory, it is loaded automatically. Command-line flags override the file.

```
[debug]
/private/tmp/pathfinder.log

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

```bash
.build/pathfinder --conf pathfinder.cfg --workers 64
```

Startup connects to `[node]` and requires `server_info.info.server_state` to be `full`, `proposing`, or `unknown`. Anything else (`syncing`, `connected`, …) exits immediately with a fatal error — it will not listen or snapshot.

Point clients at `ws://127.0.0.1:6008` or `http://127.0.0.1:5008`. Wait for `snapshot ready` on stderr (or `path_info.info.server_state = full`) before expecting path results. A full mainnet snapshot is ~19M objects and takes a few minutes. `server_info` is forwarded to the upstream node.

| Flag / stanza | Meaning |
| --- | --- |
| `--conf` / file path | Config file |
| `[node]` / `--node` | Upstream `xrpld` WebSocket |
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

`PATHFINDER_NODE` overrides `[node]` unless `--node` is also passed.

Default is a full ledger sync and a full book-graph search. Set `[search-fast]` below `[search]` to start WebSocket replies shallow and deepen while the socket stays open.

## API (same as xrpld)

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

Create returns a first (shallow) result. Later unsolicited frames have `"type": "path_find"` about every 100ms (reprice) and after each closed ledger. Hop width increases while the socket stays open. `close` / `status` work as on xrpld.

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

**`path_counts`** (local PathFinder counters; `source` is `"pathfinder"`). **`get_counts`** is forwarded to `[node]`.

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
include/pathfinder/     sidecar (ledger, graph, engine, WS/HTTP)
include/xrpld/rpc/      AssetCache / Pathfinder headers (include-path shim)
src/pathfinder/         sidecar implementation
src/vendor/path/        AssetCache, TrustLine, AccountAssets (and unused Pathfinder copy)
src/app/main.cpp
tests/pathfinder_tests.cpp
```

Live search is `FastPathFinder` in `graph.cpp` + `LocalOrderBooks`. RippleCalc still comes from `libxrpl`.
