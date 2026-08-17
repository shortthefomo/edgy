#!/usr/bin/env bash
# In-container build. Same job as xahaud/build-core.sh: Conan + CMake +
# strip + write release-build/. Called by release-builder.sh.

set -euo pipefail

CORES="${BUILD_CORES:-$(nproc)}"
EDGY_ROOT="${EDGY_ROOT:-/src/edgy}"
RIPPLED_ROOT="${RIPPLED_ROOT:-/src/rippled}"
XAHAUD_ROOT="${XAHAUD_ROOT:-/src/xahaud}"
BUILD_TYPE=Release

export TMPDIR="${TMPDIR:-/tmp}"
export CCACHE_DIR=/cache/ccache
export CONAN_HOME=/cache/conan2
# Do not set CC/CXX to "ccache gcc": Conan's detect_api then sees no compiler,
# and rippled's Jinja default profile fails to render. ccache is the CMake launcher.
unset CC CXX || true
export LDFLAGS="-static-libstdc++"
export CMAKE_EXE_LINKER_FLAGS="-static-libstdc++"
export CMAKE_C_COMPILER_LAUNCHER=ccache
export CMAKE_CXX_COMPILER_LAUNCHER=ccache

SOURCES_CACHE=/cache/conan-sources
mkdir -p "$CCACHE_DIR" "$CONAN_HOME/profiles" "$SOURCES_CACHE" "${EDGY_ROOT}/release-build"
ccache -M 20G
ccache -o cache_dir="$CCACHE_DIR"

git config --global --add safe.directory '*' || true

# Plain profile. Never use `conan profile detect` or rippled's Jinja `default`
# (that template probes CC/CXX and overwrites default on `conan/init.sh`).
write_edgy_profile() {
    local path="$1"
    local cppstd="$2"
    cat >"$path" <<EOF
[settings]
arch=x86_64
build_type=Release
compiler=gcc
compiler.cppstd=${cppstd}
compiler.libcxx=libstdc++11
compiler.version=15
os=Linux

[conf]
tools.build:compiler_executables={'c':'gcc','cpp':'g++'}
tools.cmake.cmaketoolchain:generator=Ninja
EOF
}

PROFILE_CXX23="${CONAN_HOME}/profiles/edgy-linux"
PROFILE_CXX20="${CONAN_HOME}/profiles/edgy-linux-cxx20"
write_edgy_profile "$PROFILE_CXX23" 23
write_edgy_profile "$PROFILE_CXX20" 20

echo "=== ${PROFILE_CXX23} ==="
cat "$PROFILE_CXX23"

# rippled's global.conf sets core.download:parallel=nproc and only 5 retries.
# Parallel GitHub archive hits get 429'd. Serialize + retry + token.
apply_conan_net_conf() {
    local gc="${CONAN_HOME}/global.conf"
    mkdir -p "$CONAN_HOME"
    touch "$gc"
    chmod u+w "$gc" 2>/dev/null || true
    local tmp
    tmp="$(mktemp)"
    grep -v -E '^(core\.download:parallel|tools\.files\.download:retry|tools\.files\.download:retry_wait|core\.sources:download_cache)=' \
        "$gc" >"$tmp" || true
    cat >>"$tmp" <<EOF
core.download:parallel=1
tools.files.download:retry=20
tools.files.download:retry_wait=20
core.sources:download_cache=${SOURCES_CACHE}
EOF
    mv "$tmp" "$gc"
}

# Put a GitHub archive in Conan's sha256 download cache so source() does not
# hit api.github.com again. Alternate URLs avoid the same 429 bucket.
prefetch_sha256() {
    local sha="$1"
    shift
    local dest="${SOURCES_CACHE}/${sha}"
    if [[ -f "$dest" ]] && echo "${sha}  ${dest}" | sha256sum -c --status; then
        echo "-- cache hit ${sha}"
        return 0
    fi
    local auth=()
    if [[ -n "${GITHUB_TOKEN:-}" ]]; then
        auth=(-H "Authorization: Bearer ${GITHUB_TOKEN}" -H "X-GitHub-Api-Version: 2022-11-28")
        echo "-- prefetch using GITHUB_TOKEN"
    else
        echo "-- prefetch without token (set GITHUB_TOKEN or run \`gh auth login\`)"
    fi
    local url tmp
    tmp="$(mktemp)"
    for url in "$@"; do
        echo "-- prefetch ${url}"
        if curl -fL --retry 8 --retry-delay 15 --retry-all-errors \
            "${auth[@]}" -o "$tmp" "$url"; then
            if echo "${sha}  ${tmp}" | sha256sum -c --status; then
                mv "$tmp" "$dest"
                echo "-- stored ${dest}"
                return 0
            fi
            echo "-- checksum mismatch for ${url}"
        fi
    done
    rm -f "$tmp"
    echo "-- prefetch failed for ${sha} (conan will retry)"
    return 1
}

apply_conan_net_conf

# Known GitHub archives that Conan source() pulls for this tree.
prefetch_sha256 dc8c167f48f3de5ae318c528b26b72f300edb6e33744e55394674fd4b7cdd21d \
    "https://codeload.github.com/ianlancetaylor/libbacktrace/tar.gz/dedbe13fda00253fe5d4f2fb812c909729ed5937" \
    "https://github.com/ianlancetaylor/libbacktrace/archive/dedbe13fda00253fe5d4f2fb812c909729ed5937.tar.gz" \
    || true

conan_install_retry() {
    local n=1
    local max=6
    while true; do
        if conan install "$@"; then
            return 0
        fi
        if ((n >= max)); then
            echo "ERR: conan install failed after ${max} attempts" >&2
            return 1
        fi
        echo "-- conan install failed (try ${n}/${max}); waiting $((20 * n))s"
        sleep $((20 * n))
        n=$((n + 1))
    done
}

build_libxrpl() {
    local root="$1"
    local extra_conan="${2:-}"
    local label="$3"
    local dest="${root}/.build-linux"

    echo "=== ${label}: libxrpl (${dest}) ==="
    if [[ -x "${root}/conan/init.sh" ]]; then
        (cd "$root" && ./conan/init.sh)
        # init.sh replaces profiles/default (Jinja) and rewrites global.conf.
        write_edgy_profile "$PROFILE_CXX23" 23
        write_edgy_profile "$PROFILE_CXX20" 20
        apply_conan_net_conf
    fi
    if [[ -d "${root}/external/snappy" ]]; then
        conan export "${root}/external/snappy" --version 1.1.10 --user xahaud --channel stable
        conan export "${root}/external/soci" --version 4.0.3 --user xahaud --channel stable
    fi

    mkdir -p "$dest"
    local profile="${PROFILE_CXX23}"
    if [[ "$label" == xahaud ]]; then
        profile="${PROFILE_CXX20}"
    fi
    (
        cd "$dest"
        # shellcheck disable=SC2086
        conan_install_retry "$root" --output-folder . --build missing \
            --profile:all="$profile" \
            --settings "build_type=${BUILD_TYPE}" \
            ${extra_conan}
        cmake -G Ninja \
            -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
            -DCMAKE_C_COMPILER=gcc \
            -DCMAKE_CXX_COMPILER=g++ \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            -DCMAKE_TOOLCHAIN_FILE:FILEPATH=build/generators/conan_toolchain.cmake \
            -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++" \
            -Dxrpld=OFF \
            -Dtests=OFF \
            "$root"
        cmake --build . --target xrpl.libxrpl --parallel "$CORES"
    )
    if [[ ! -f "${dest}/libxrpl.a" ]]; then
        echo "ERR: ${dest}/libxrpl.a missing after ${label} build" >&2
        find "$dest" -name 'libxrpl.a' -o -name 'libxrpl.*' | head
        exit 1
    fi
    echo "=== ${label} libxrpl.a ok ($(du -h "${dest}/libxrpl.a" | awk '{print $1}')) ==="
}

if [[ ! -f "${RIPPLED_ROOT}/include/xrpl/tx/paths/RippleCalc.h" ]]; then
    echo "ERR: RIPPLED_ROOT=${RIPPLED_ROOT} is not a rippled tree" >&2
    exit 1
fi
if [[ ! -f "${XAHAUD_ROOT}/src/xrpld/app/paths/RippleCalc.cpp" ]]; then
    echo "ERR: XAHAUD_ROOT=${XAHAUD_ROOT} is not a xahaud tree" >&2
    exit 1
fi

build_libxrpl "$RIPPLED_ROOT" "" "rippled"
# xahaud is C++20 and must not pull WasmEdge into libxrpl.
build_libxrpl "$XAHAUD_ROOT" \
    "--settings compiler.cppstd=20 -o with_wasmedge=False -o xrpld=False -o tests=False" \
    "xahaud"

echo "=== edgy ==="
mkdir -p "${EDGY_ROOT}/.build-linux"
(
    cd "${EDGY_ROOT}/.build-linux"
    cmake -G Ninja \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_TOOLCHAIN_FILE="${RIPPLED_ROOT}/.build-linux/build/generators/conan_toolchain.cmake" \
        -DCMAKE_C_COMPILER=gcc \
        -DCMAKE_CXX_COMPILER=g++ \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++" \
        -DRIPPLED_ROOT="${RIPPLED_ROOT}" \
        -DRIPPLED_BUILD="${RIPPLED_ROOT}/.build-linux" \
        -DXAHAUD_ROOT="${XAHAUD_ROOT}" \
        -DXAHAUD_BUILD="${XAHAUD_ROOT}/.build-linux" \
        "${EDGY_ROOT}"
    cmake --build . --target edgy-xrpld edgy-xahaud edgy_tests --parallel "$CORES"
    ./edgy_tests
)

ver_base="$(
    sed -n 's/.*kVersionBase = "\([^"]*\)".*/\1/p' "${EDGY_ROOT}/include/edgy/version.hpp" | head -1
)"
[[ -n "$ver_base" ]] || ver_base=dev

arch="$(uname -m)"
case "$arch" in
    x86_64 | amd64) arch_tag=linux-x64 ;;
    aarch64 | arm64) arch_tag=linux-arm64 ;;
    *) arch_tag="linux-${arch}" ;;
esac

out="${EDGY_ROOT}/release-build"
xrpld_out="${out}/edgy-xrpld-${ver_base}-${arch_tag}"
xahaud_out="${out}/edgy-xahaud-${ver_base}-${arch_tag}"
strip -s "${EDGY_ROOT}/.build-linux/edgy-xrpld" "${EDGY_ROOT}/.build-linux/edgy-xahaud"
cp "${EDGY_ROOT}/.build-linux/edgy-xrpld" "$xrpld_out"
cp "${EDGY_ROOT}/.build-linux/edgy-xahaud" "$xahaud_out"
chmod +x "$xrpld_out" "$xahaud_out"

{
    echo "Build host: $(hostname)"
    echo "Build date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Platform: linux/${arch}"
    echo "Compiler: $(gcc --version | head -1)"
    echo "edgy-xrpld: $("${xrpld_out}" --version)"
    echo "edgy-xahaud: $("${xahaud_out}" --version)"
    echo "ldd edgy-xrpld:"
    ldd "$xrpld_out" || true
    echo "ldd edgy-xahaud:"
    ldd "$xahaud_out" || true
} | tee "${out}/release.info"

ccache -s
echo "END INSIDE CONTAINER"
