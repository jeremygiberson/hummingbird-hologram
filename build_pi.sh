#!/usr/bin/env bash
#
# build_pi.sh — Native build of the hologram binary on Raspberry Pi OS.
#
# Builds the project in-tree (GLES2 path is auto-selected by CMake on ARM)
# and wires /usr/share/hologram -> this source tree so the binary can find
# its shaders/ and assets/ (PLATFORM_PI hardcodes that base path; see
# src/platform.h). Run from anywhere; paths are derived from the script.
#
# Usage:
#   ./build_pi.sh            # Debug build (default)
#   ./build_pi.sh Release    # Release build

set -euo pipefail

BUILD_TYPE="${1:-Debug}"
PROJECT_DIR="/home/hummingbird/Desktop/hummingbird-hologram"
BUILD_DIR="${PROJECT_DIR}/build"
ASSET_LINK="/usr/share/hologram"

echo "==> Project:    ${PROJECT_DIR}"
echo "==> Build type: ${BUILD_TYPE}"

# --- 1. Dependency check (informational; install if missing) -------------
missing=()
for pkg in libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev cmake build-essential; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        missing+=("$pkg")
    fi
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "==> Installing missing packages: ${missing[*]}"
    sudo apt update
    sudo apt install -y "${missing[@]}"
else
    echo "==> All build dependencies present."
fi

# --- 2. Configure + build ------------------------------------------------
mkdir -p "${BUILD_DIR}"
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

# --- 3. Asset path symlink (idempotent) ----------------------------------
if [ -L "${ASSET_LINK}" ]; then
    current="$(readlink -f "${ASSET_LINK}")"
    if [ "${current}" = "${PROJECT_DIR}" ]; then
        echo "==> Symlink ${ASSET_LINK} already points here. OK."
    else
        echo "==> Repointing existing symlink ${ASSET_LINK} (was: ${current})"
        sudo ln -sfn "${PROJECT_DIR}" "${ASSET_LINK}"
    fi
elif [ -e "${ASSET_LINK}" ]; then
    echo "ERROR: ${ASSET_LINK} exists and is NOT a symlink (likely from 'make install')."
    echo "       Inspect it and remove if safe:  sudo rm -rf ${ASSET_LINK}"
    echo "       Then re-run this script."
    exit 1
else
    echo "==> Creating symlink ${ASSET_LINK} -> ${PROJECT_DIR}"
    sudo ln -s "${PROJECT_DIR}" "${ASSET_LINK}"
fi

# --- 4. Verify the symlink resolves to real assets -----------------------
echo "==> Verifying asset paths..."
ok=true
for sub in shaders assets; do
    if [ -d "${ASSET_LINK}/${sub}" ]; then
        count="$(find "${ASSET_LINK}/${sub}" -maxdepth 1 -type f | wc -l | tr -d ' ')"
        echo "    ${ASSET_LINK}/${sub}  ->  $(readlink -f "${ASSET_LINK}/${sub}")  (${count} files)"
    else
        echo "    MISSING: ${ASSET_LINK}/${sub}"
        ok=false
    fi
done

if [ "${ok}" != true ]; then
    echo "ERROR: asset directories did not resolve. The binary will not find shaders/assets."
    exit 1
fi

echo
echo "==> Done. Run it with:"
echo "    ${BUILD_DIR}/hologram"
