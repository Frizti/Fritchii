#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Fritchii contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds Fritchii-v<N>-Linux-x86_64.AppImage and .tar.gz in an Ubuntu 24.04 container (clang,
# Qt 6.8.3). Needs only Docker. The packages land in build/.
#   tools/appimage/build.sh
#   JOBS=<n>          compile jobs (default: the cores, at most one per 2 GB of free RAM)
#   RECIPES_ZIP=<zip> a Fritchii Windows release zip to take the shader recipes from (default: the
#                     release of this version is downloaded; see tools/appimage/package.sh)
#   WERROR=ON         build with -Werror (default OFF)
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
image=fritchii-appimage-builder

if [ "${1:-}" != "--inside-container" ]; then
    if [ -z "${JOBS:-}" ]; then
        free_gb=$(awk '/MemAvailable/ { print int($2 / 1048576) }' /proc/meminfo)
        JOBS=$(( free_gb / 2 ))
        [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
        [ "$JOBS" -lt 1 ] && JOBS=1
    fi
    docker build -t "$image" "$root/tools/appimage"
    mkdir -p "$root/build/appimage-home"
    recipes=()
    if [ -n "${RECIPES_ZIP:-}" ]; then
        recipes=(-v "$(realpath "$RECIPES_ZIP"):/recipes.zip:ro" -e RECIPES_ZIP=/recipes.zip)
    fi
    exec docker run --rm --user "$(id -u):$(id -g)" \
        -e HOME=/src/build/appimage-home -e JOBS="$JOBS" -e WERROR="${WERROR:-OFF}" \
        -e RECIPES_VERSION="${RECIPES_VERSION:-}" -e ALLOW_NO_RECIPES="${ALLOW_NO_RECIPES:-0}" \
        "${recipes[@]}" \
        -v "$root:/src" -w /src "$image" tools/appimage/build.sh --inside-container
fi

build=build/appimage
# FRITCHII_APPIMAGE: the updater installs the AppImage of a newer fritchii-v* release in place.
# Configure once: a second configure cannot find the bundled OpenSSL.
[ -f "$build/build.ninja" ] || cmake -S . -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DYUZU_TESTS=OFF \
    -DYUZU_USE_BUNDLED_QT=OFF \
    -DCMAKE_PREFIX_PATH="$QT_ROOT_DIR" \
    -DYUZU_USE_BUNDLED_SDL3=ON \
    -DVulkanHeaders_FORCE_BUNDLED=ON \
    -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
    -DENABLE_UPDATE_CHECKER=ON \
    -DFRITCHII_APPIMAGE=ON \
    -DENABLE_WERROR="${WERROR:-OFF}" \
    -DLICHII_BUILD_PLATFORM=linux-x86_64 \
    -DUSE_DISCORD_PRESENCE=OFF \
    -DCMAKE_C_COMPILER="clang-$LLVM_VERSION" \
    -DCMAKE_CXX_COMPILER="clang++-$LLVM_VERSION" \
    -DCMAKE_EXE_LINKER_FLAGS="-B/usr/lib/llvm-$LLVM_VERSION/bin -fuse-ld=lld"
cmake --build "$build" -j "$JOBS" --target yuzu

BUILD_DIR="$PWD/$build" OUT_DIR="$PWD/build" TOOLS_DIR="$PWD/build/appimage-tools" \
    tools/appimage/package.sh
