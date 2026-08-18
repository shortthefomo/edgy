| :warning: **WARNING** :warning: |
| ------------------------------- |
| These instructions assume you have a C++ development environment ready with Git, Python, Conan, CMake, and a C++23 compiler. Edgy links `libxrpl` and reuses the Conan toolchain from a sibling [rippled](https://github.com/XRPLF/rippled) tree — you must build that first. For help setting up the toolchain on Linux, macOS, or Windows, see rippled’s [environment guide](https://github.com/XRPLF/rippled/blob/develop/docs/build/environment.md). For Conan and CMake background, see their [crash course](https://github.com/XRPLF/rippled/blob/develop/docs/build/conan.md) or the official [Getting Started][conan-getting-started] walkthrough. |

## Minimum Requirements

Edgy is C++23 and requires:

| Tool | Minimum |
| --- | --- |
| [CMake](https://cmake.org/download/) | 3.20 |
| C++ compiler with C++23 | same versions rippled tests (GCC 15, Clang 22, Apple Clang 21, MSVC / VS 2026) |
| A built rippled tree | `libxrpl.a` plus Conan’s generated toolchain file |

Hardware is whatever you already use to build rippled. A full mainnet snapshot at runtime is ~19 million ledger objects and needs tens of gigabytes of RAM; that is a run-time cost, not a compile-time one.

## Operating Systems

Same support as rippled:

- **Linux** — Ubuntu is the best-tested distro. Debian and Red Hat also work.
- **macOS** — used for development. Minimum is macOS 15 (Sequoia).
- **Windows** — development only. Use a rippled build directory produced with the Visual Studio generator.

## Layout

CMake looks for rippled as a **sibling** of this repository by default:

```
Ledgers/
  rippled/          XRPL reference implementation
    .build/         rippled CMake + Conan output (must contain libxrpl.a)
  xahaud/           Xahau node (https://github.com/Xahau/xahaud)
    .build/         xahaud CMake output (must contain libxrpl.a)
  edgy/             this repository
    .build/         Edgy CMake output
```

One CMake build produces **two** binaries. They cannot share a single `libxrpl`:

| Binary | Links | Talks to |
| --- | --- | --- |
| `edgy-xrpld` | rippled `libxrpl.a` | `xrpld` |
| `edgy-xahaud` | xahaud `libxrpl.a` plus xahaud RippleCalc/Flow sources | `xahaud` |

Override the locations with `-DRIPPLED_ROOT=` / `-DRIPPLED_BUILD=` and `-DXAHAUD_ROOT=` / `-DXAHAUD_BUILD=` if your trees are elsewhere.

## Steps

### 1. Build rippled / libxrpl

Edgy does not vendor Boost, OpenSSL, secp256k1, and the rest. It finds those packages through the Conan toolchain file that rippled’s `conan install` already wrote.

In the rippled tree, follow [rippled `BUILD.md`](https://github.com/XRPLF/rippled/blob/develop/BUILD.md):

```bash
cd ../rippled
./conan/init.sh
mkdir -p .build && cd .build
conan install .. --output-folder . --build missing --settings build_type=Release
cmake -DCMAKE_TOOLCHAIN_FILE:FILEPATH=build/generators/conan_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -Dxrpld=ON \
      -Dtests=OFF \
      ..
cmake --build . --parallel
```

You only need `libxrpl.a`. `-Dxrpld=ON` also builds the `xrpld` binary, which is useful if you will run a local node for Edgy to snapshot. To build just the library:

```bash
cmake -DCMAKE_TOOLCHAIN_FILE:FILEPATH=build/generators/conan_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -Dxrpld=OFF \
      -Dtests=OFF \
      ..
cmake --build . --target xrpl.libxrpl --parallel
```

When this step is done you should have:

```
../rippled/.build/libxrpl.a
../rippled/.build/build/generators/conan_toolchain.cmake
```

Use the same `CMAKE_BUILD_TYPE` (Release or Debug) for Edgy as you passed to Conan and to rippled. Mixing them is the usual cause of missing protobuf headers and ABI errors.

A **Release** binary is the optimized one you run against a live node. A **Debug** binary is for stepping in a debugger and is much slower. rippled uses the same split: Conan’s `build_type` and CMake’s `CMAKE_BUILD_TYPE` must both be `Release`.

### 1b. Build xahaud / libxrpl

Same idea in the xahaud tree (its own Conan + CMake). You need `../xahaud/.build/libxrpl.a`. Edgy compiles xahaud’s RippleCalc/Flow sources from `XAHAUD_ROOT` and links that library — path finding is not in xahaud’s `libxrpl`.

Use the same `CMAKE_BUILD_TYPE` as rippled and Edgy.

### 2. Configure Edgy

From the Edgy repository root:

```bash
cmake -S . -B .build \
  -DCMAKE_TOOLCHAIN_FILE=../rippled/.build/build/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DRIPPLED_ROOT=../rippled \
  -DRIPPLED_BUILD=../rippled/.build \
  -DXAHAUD_ROOT=../xahaud \
  -DXAHAUD_BUILD=../xahaud/.build
```

If `CMAKE_TOOLCHAIN_FILE` is omitted, CMake will pick
`${RIPPLED_ROOT}/.build/build/generators/conan_toolchain.cmake` when that file exists. Passing it explicitly is clearer when the rippled build directory is not the default. Both node trees still have to be built first.

Single-config generators (`Unix Makefiles`, `Ninja`) need `CMAKE_BUILD_TYPE`. Multi-config generators (Visual Studio, Xcode) select the config at build time instead.

You can use any build directory name. `.build` matches rippled’s convention and is listed in `.gitignore`.

### 3. Build

Single-config:

```bash
cmake --build .build --parallel
```

Multi-config:

```bash
cmake --build .build --config Release --parallel
```

Replace `--parallel` with `--parallel N` (or `-j N`) if you want to cap the job count. A common starting point is half the number of CPU cores.

This produces:

| Target | Output |
| --- | --- |
| `edgy-xrpld` | `.build/edgy-xrpld` — sidecar linked to rippled libxrpl |
| `edgy-xahaud` | `.build/edgy-xahaud` — sidecar linked to xahaud libxrpl |
| `edgy_tests` | `.build/edgy_tests` — unit tests (rippled libxrpl) |

Build one target:

```bash
cmake --build .build --parallel --target edgy-xrpld
cmake --build .build --parallel --target edgy-xahaud
cmake --build .build --parallel --target edgy_tests
```

### 4. Test

```bash
ctest --test-dir .build --output-on-failure
```

or run the binary directly:

```bash
.build/edgy_tests
```

On a multi-config generator the binaries sit under `.build/Release/` (or `.build/Debug/`).

### 5. Run

Configuration and the RPC/WS API are documented in [`README.md`](README.md). After a successful build:

```bash
cp cfg/edgy-xrpl.example.cfg edgy-xrpl.cfg
cp cfg/edgy-xahau.example.cfg edgy-xahau.cfg
# edit [node] in each file to your node WebSocket
.build/edgy-xrpld --conf edgy-xrpl.cfg
.build/edgy-xahaud --conf edgy-xahau.cfg
```

Startup requires the upstream `server_state` to be `full`, `proposing`, or `unknown`. Wait for `snapshot ready` on stderr (or `path_info.info.server_state = full`) before sending `path_find`.

Pick the binary that matches the node. The two `libxrpl`s cannot live in one process. `edgy-xahaud` compiles xahaud RippleCalc/Flow from `XAHAUD_ROOT` and links xahaud `libxrpl.a`, so Hook / URIToken / HookState are known types. Native JSON is `XAH`. The book graph still uses offers, AMMs, and directories.

## Release build

This is the rippled-shaped path: Conan `Release` + CMake `Release`, then an identifiable binary.

### Optimized binary (what you run)

1. Build rippled as Release (step 1 above, `build_type=Release`).
2. Configure and build Edgy as Release (steps 2–3). The commands already pass `-DCMAKE_BUILD_TYPE=Release`.
3. Check it:

```bash
.build/edgy-xrpld --version
# edgy 0.1.7+xrpld.<git>
.build/edgy-xahaud --version
# edgy 0.1.7+xahaud.<git>
```

A Debug Edgy prints `+DEBUG` in the version (same idea as `xrpld --version`). Do not mix a Release Edgy with a Debug `libxrpl.a`.

Install like rippled’s `cmake --install`:

```bash
cmake --install .build --prefix /usr/local
# /usr/local/bin/edgy-xrpld
# /usr/local/bin/edgy-xahaud
# /usr/local/etc/edgy/edgy-xrpl.example.cfg
# /usr/local/etc/edgy/edgy-xahau.example.cfg
```

On a multi-config generator (Visual Studio / Xcode):

```bash
cmake --build .build --config Release --parallel --target edgy-xrpld edgy-xahaud
cmake --install .build --config Release --prefix /usr/local
```

### Tagged product release (later)

rippled also has a *product* release: bump `versionString` in `BuildInfo.cpp`, tag `3.2.0`, push, GitHub Release, then CI builds `.deb` / `.rpm`. Edgy should follow a smaller copy of that, not the packaging farm:

| rippled | Edgy |
| --- | --- |
| Work on `develop` | Same |
| `versionString = "X.Y.Z-bN"` on develop | `kVersionBase` in `include/edgy/version.hpp` |
| Signed commit “Set version to X.Y.Z” | Same, edit `kVersionBase` and `project(edgy VERSION …)` |
| Tag `X.Y.Z`, GitHub Release | Tag `vX.Y.Z`, attach `.build/edgy-xrpld` and `.build/edgy-xahaud` |
| `package/` deb/rpm + `on-tag.yml` | Skip until there is more than one installer |

### Linux x86_64 (xahaud-style Docker builder)

xahaud ships Linux by running Conan + CMake inside Docker and copying a stripped binary out. Edgy copies that wrapper, not their Holy Build Box image — HBB is GCC 11 / C++20, and this tree plus current rippled need **GCC 15 / C++23**.

From the repo root, with Docker:

```bash
./release-builder.sh
# release-build/edgy-xrpld-<ver>-linux-x64
# release-build/edgy-xahaud-<ver>-linux-x64
```

Uses `../rippled` and `../xahaud` when those trees exist; otherwise clones `XRPLF/rippled@develop` and `Xahau/xahaud@dev` into `.deps/`. Upstream object files go in each tree’s `.build-linux/` so a macOS `.build/` is left alone.

On Apple Silicon, Docker builds `linux/amd64` (qemu). That is slow; the GitHub Action `Linux release` is the native path. It runs on `v*` tags and `workflow_dispatch`, then attaches `*-linux-x64` to the GitHub Release.

`PLATFORM=linux/arm64 ./release-builder.sh` builds `*-linux-arm64` instead.

Current release is `0.1.7`. After that tag, develop is `0.1.8-b6`. To cut the next release:

```bash
# 1. set kVersionBase to "X.Y.Z" (and project(edgy VERSION X.Y.Z) if the
#    CMake project version should match)
# 2. signed commit: Set version to X.Y.Z
# 3. cmake Release rebuild, run edgy_tests, both binaries --version
# 4. git tag -s vX.Y.Z -m "edgy X.Y.Z"
# 5. git push origin develop --tags
```

## CMake options

| Option | Default | Description |
| --- | --- | --- |
| `RIPPLED_ROOT` | `${CMAKE_SOURCE_DIR}/../rippled` | rippled source tree (headers under `include/xrpl`) |
| `RIPPLED_BUILD` | `${RIPPLED_ROOT}/.build` | rippled build tree that contains `libxrpl.a` and Conan generators |
| `XAHAUD_ROOT` | `${CMAKE_SOURCE_DIR}/../xahaud` | xahaud source tree (RippleCalc / Flow live under `src/xrpld`) |
| `XAHAUD_BUILD` | `${XAHAUD_ROOT}/.build` | xahaud build tree that contains `libxrpl.a` |
| `CMAKE_TOOLCHAIN_FILE` | `${RIPPLED_ROOT}/.build/build/generators/conan_toolchain.cmake` if present | Conan toolchain; must match the rippled install |
| `CMAKE_BUILD_TYPE` | (generator default) | `Release` or `Debug`; must match Conan’s `build_type` |

There is no Edgy-specific Conan recipe. Dependencies (`Boost`, `OpenSSL`, `ZLIB`, `secp256k1`, `ed25519`, `xxHash`, `date`, `FastFloat`, `mpt-crypto`, `lz4`, `LibArchive`) come from the rippled toolchain.

## Troubleshooting

### `libxrpl.a not found`

```
libxrpl.a not found in RIPPLED_BUILD=...
Build rippled first.
```

`RIPPLED_BUILD` does not point at a completed rippled CMake build. Build rippled (step 1) or pass the directory that actually contains `libxrpl.a`.

### `RIPPLED_ROOT does not look like rippled`

CMake checks for `include/xrpl/tx/paths/RippleCalc.h`. Point `RIPPLED_ROOT` at the rippled **source** tree, not the `.build` directory.

### `Could not find a package configuration file` (Boost, secp256k1, …)

The Conan toolchain was not passed, or it was generated for a different rippled checkout. Re-run rippled’s `conan install`, then reconfigure Edgy with `-DCMAKE_TOOLCHAIN_FILE=.../conan_toolchain.cmake`.

### `protobuf/port_def.inc` file not found

The Edgy `CMAKE_BUILD_TYPE` does not match the `build_type` used for `conan install` / the rippled build. For Debug:

```bash
# in rippled/.build
conan install .. --output-folder . --build missing --settings build_type=Debug
cmake -DCMAKE_TOOLCHAIN_FILE:FILEPATH=build/generators/conan_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . --parallel

# in Edgy
cmake -S . -B .build \
  -DCMAKE_TOOLCHAIN_FILE=../rippled/.build/build/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRIPPLED_ROOT=../rippled \
  -DRIPPLED_BUILD=../rippled/.build
cmake --build .build --parallel
```

### After updating rippled

A new rippled revision can change headers or `libxrpl.a`. Rebuild rippled, then reconfigure Edgy (a clean `.build` is safest if CMake cache still points at old include paths):

```bash
rm -rf .build
cmake -S . -B .build \
  -DCMAKE_TOOLCHAIN_FILE=../rippled/.build/build/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DRIPPLED_ROOT=../rippled \
  -DRIPPLED_BUILD=../rippled/.build
cmake --build .build --parallel
```

If Conan packages themselves changed, follow rippled’s [Conan troubleshooting](https://github.com/XRPLF/rippled/blob/develop/BUILD.md#troubleshooting) (`conan remove`, re-export, lockfile) before rebuilding Edgy.

[conan-getting-started]: https://docs.conan.io/en/latest/getting_started.html
[build_type]: https://cmake.org/cmake/help/latest/variable/CMAKE_BUILD_TYPE.html
