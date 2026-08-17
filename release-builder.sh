#!/usr/bin/env bash
# Host wrapper. Copied from xahaud/release-builder.sh:
#   Docker image → in-container build-core.sh → release-build/<binary>
# Run from the Edgy repo root. Needs Docker.
#
# Optional env:
#   PLATFORM          docker platform (default linux/amd64)
#   BUILD_CORES       compile jobs (default nproc)
#   RIPPLED_ROOT      existing rippled tree (default ../rippled or clone)
#   XAHAUD_ROOT       existing xahaud tree (default ../xahaud or clone)
#   RIPPLED_REPO/REF  used only when RIPPLED_ROOT is missing
#   XAHAUD_REPO/REF   used only when XAHAUD_ROOT is missing
#   GITHUB_TOKEN      optional; avoids GitHub 429 on Conan source tarballs
#                     (defaults to \`gh auth token\` when that works)

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

if ! command -v docker >/dev/null 2>&1; then
    echo 'Docker not found. Install it first.' >&2
    exit 1
fi

PLATFORM="${PLATFORM:-linux/amd64}"
if command -v nproc >/dev/null 2>&1; then
    BUILD_CORES="${BUILD_CORES:-$(nproc)}"
else
    BUILD_CORES="${BUILD_CORES:-8}"
fi

RIPPLED_REPO="${RIPPLED_REPO:-https://github.com/XRPLF/rippled.git}"
RIPPLED_REF="${RIPPLED_REF:-develop}"
XAHAUD_REPO="${XAHAUD_REPO:-https://github.com/Xahau/xahaud.git}"
XAHAUD_REF="${XAHAUD_REF:-dev}"

ensure_tree() {
    local dest="$1" repo="$2" ref="$3" name="$4"
    if [[ -f "${dest}/CMakeLists.txt" ]]; then
        echo "-- using existing ${name} at ${dest}"
        return
    fi
    echo "-- cloning ${name} ${repo} @ ${ref} -> ${dest}"
    mkdir -p "$(dirname "$dest")"
    git clone --depth 1 --branch "$ref" "$repo" "$dest"
}

if [[ -z "${RIPPLED_ROOT:-}" ]]; then
    if [[ -f "${ROOT}/../rippled/CMakeLists.txt" ]]; then
        RIPPLED_ROOT="$(cd "${ROOT}/../rippled" && pwd)"
    else
        RIPPLED_ROOT="${ROOT}/.deps/rippled"
    fi
fi
if [[ -z "${XAHAUD_ROOT:-}" ]]; then
    if [[ -f "${ROOT}/../xahaud/CMakeLists.txt" ]]; then
        XAHAUD_ROOT="$(cd "${ROOT}/../xahaud" && pwd)"
    else
        XAHAUD_ROOT="${ROOT}/.deps/xahaud"
    fi
fi

ensure_tree "$RIPPLED_ROOT" "$RIPPLED_REPO" "$RIPPLED_REF" rippled
ensure_tree "$XAHAUD_ROOT" "$XAHAUD_REPO" "$XAHAUD_REF" xahaud

IMAGE_NAME="${IMAGE_NAME:-edgy-linux-builder:latest}"
CACHE_VOLUME="${CACHE_VOLUME:-edgy-linux-builder-cache}"

echo "-- BUILD CORES:  ${BUILD_CORES}"
echo "-- PLATFORM:     ${PLATFORM}"
echo "-- RIPPLED_ROOT: ${RIPPLED_ROOT}"
echo "-- XAHAUD_ROOT:  ${XAHAUD_ROOT}"
echo "-- IMAGE:        ${IMAGE_NAME}"

rm -rf "${ROOT}/release-build"
mkdir -p "${ROOT}/release-build"

echo "-- building Docker image"
docker build --platform "$PLATFORM" -t "$IMAGE_NAME" -f docker/linux-builder.Dockerfile docker

docker volume create "$CACHE_VOLUME" >/dev/null

if [[ -z "${GITHUB_TOKEN:-}" ]] && command -v gh >/dev/null 2>&1; then
    GITHUB_TOKEN="$(gh auth token 2>/dev/null || true)"
fi
if [[ -n "${GITHUB_TOKEN:-}" ]]; then
    echo "-- GITHUB_TOKEN: set (Conan source downloads)"
else
    echo "-- GITHUB_TOKEN: unset (GitHub may 429 source tarballs)"
fi

echo "-- starting container"
docker run --rm --user 0:0 --platform "$PLATFORM" \
    -e BUILD_CORES="$BUILD_CORES" \
    -e GITHUB_TOKEN="${GITHUB_TOKEN:-}" \
    -v "${ROOT}:/src/edgy" \
    -v "${RIPPLED_ROOT}:/src/rippled" \
    -v "${XAHAUD_ROOT}:/src/xahaud" \
    -v "${CACHE_VOLUME}:/cache" \
    "$IMAGE_NAME" \
    bash -x /src/edgy/build-core.sh

# Container writes as root; give the files back to the invoking user.
docker run --rm --user 0:0 --platform "$PLATFORM" \
    -v "${ROOT}:/src/edgy" \
    -v "${RIPPLED_ROOT}:/src/rippled" \
    -v "${XAHAUD_ROOT}:/src/xahaud" \
    "$IMAGE_NAME" \
    chown -R "$(id -u):$(id -g)" \
        /src/edgy/release-build \
        /src/edgy/.build-linux \
        /src/rippled/.build-linux \
        /src/xahaud/.build-linux \
    || true

echo "-- DONE"
ls -lh "${ROOT}/release-build"
