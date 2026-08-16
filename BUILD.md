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
  edgy/             this repository
    .build/         Edgy CMake output
```

Override the locations with `-DRIPPLED_ROOT=` and `-DRIPPLED_BUILD=` if your trees are elsewhere.

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

### 2. Configure Edgy

From the Edgy repository root:

```bash
cmake -S . -B .build \
  -DCMAKE_TOOLCHAIN_FILE=../rippled/.build/build/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DRIPPLED_ROOT=../rippled \
  -DRIPPLED_BUILD=../rippled/.build
```

If `CMAKE_TOOLCHAIN_FILE` is omitted, CMake will pick
`${RIPPLED_ROOT}/.build/build/generators/conan_toolchain.cmake` when that file exists. Passing it explicitly is clearer when the rippled build directory is not the default.

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
| `edgy` | `.build/edgy` — the sidecar |
| `edgy_tests` | `.build/edgy_tests` — unit tests |
| `edgy_core` | static library used by both |

Build one target:

```bash
cmake --build .build --parallel --target edgy
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
cp cfg/edgy.example.cfg edgy.cfg
# edit [node] to your xrpld WebSocket
.build/edgy --conf edgy.cfg
```

Startup requires the upstream `server_state` to be `full`, `proposing`, or `unknown`. Wait for `snapshot ready` on stderr (or `path_info.info.server_state = full`) before sending `path_find`.

## CMake options

| Option | Default | Description |
| --- | --- | --- |
| `RIPPLED_ROOT` | `${CMAKE_SOURCE_DIR}/../rippled` | rippled source tree (headers under `include/xrpl`) |
| `RIPPLED_BUILD` | `${RIPPLED_ROOT}/.build` | rippled build tree that contains `libxrpl.a` and Conan generators |
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
