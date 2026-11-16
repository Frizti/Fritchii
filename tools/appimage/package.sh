#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Fritchii contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Packs a finished Linux build into Fritchii-v<version>-Linux-x86_64.AppImage and a self-contained
# Fritchii-v<version>-Linux-x86_64.tar.gz made from the same AppDir. Used by tools/appimage/build.sh
# (inside its container) and by .github/workflows/linux-build.yml.
#
# Environment:
#   BUILD_DIR        the CMake build folder (bin/lichii in it)
#   QT_ROOT_DIR      the Qt that was built against (its plugins are bundled)
#   VERSION          the version in the file names (default: dist/frizti_version.txt)
#   OUT_DIR          where the packages go (default: the current folder)
#   TOOLS_DIR        where linuxdeploy is downloaded to (default: $BUILD_DIR/../appimage-tools)
#   RECIPES_ZIP      a Fritchii Windows release zip to take the shader recipes from, or
#   RECIPES_VERSION  the release to download it from (default: VERSION). The recipes are not in
#                    git (they aren't GPL): they ship in the Windows zip only. A private repository
#                    needs RECIPES_ZIP, or gh with GH_TOKEN set.
#   ALLOW_NO_RECIPES=1  package without recipes instead of failing (no shader preparation)
#   UPDATE_INFORMATION  AppImage update information (default: the .zsync of the latest Frizti/Fritchii
#                    release), read by AppImageUpdate, Gear Lever and AppImageLauncher; empty = none.
#                    Fritchii's own updater ignores it (it takes Fritchii-v<N>-Linux-x86_64.AppImage).
#
# Next to each package goes a <file>.sha256 (sha256sum format), and next to the AppImage its .zsync.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"

: "${BUILD_DIR:?BUILD_DIR must name the CMake build folder}"
: "${QT_ROOT_DIR:?QT_ROOT_DIR must name the Qt install}"
VERSION=${VERSION:-$(tr -d '[:space:]' < dist/frizti_version.txt)}
OUT_DIR=${OUT_DIR:-$PWD}
TOOLS_DIR=${TOOLS_DIR:-$(dirname "$BUILD_DIR")/appimage-tools}
RECIPES_VERSION=${RECIPES_VERSION:-$VERSION}
exe=lichii
name="Fritchii-v$VERSION-Linux-x86_64"
appdir="$(dirname "$BUILD_DIR")/AppDir"

test -x "$BUILD_DIR/bin/$exe"
mkdir -p "$TOOLS_DIR" "$OUT_DIR"

# linuxdeploy at a fixed release, checked against its published sha256 (the continuous tag
# moves). The Qt plugin has no release with a published digest: set PLUGIN_QT_SHA256 to pin it.
LINUXDEPLOY_TAG=1-alpha-20251107-1
LINUXDEPLOY_SHA256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
PLUGIN_QT_TAG=1-alpha-20250213-1
PLUGIN_QT_SHA256=${PLUGIN_QT_SHA256:-}
fetch_tool() {
    local repo=$1 tag=$2 file=$3 sha=$4
    if [ ! -s "$TOOLS_DIR/$file" ]; then
        curl -fsSL -o "$TOOLS_DIR/$file" \
            "https://github.com/linuxdeploy/$repo/releases/download/$tag/$file"
    fi
    if [ -n "$sha" ]; then
        echo "$sha  $TOOLS_DIR/$file" | sha256sum -c -
    else
        echo "Not pinned: $(sha256sum "$TOOLS_DIR/$file")"
    fi
    chmod +x "$TOOLS_DIR/$file"
}
fetch_tool linuxdeploy "$LINUXDEPLOY_TAG" linuxdeploy-x86_64.AppImage "$LINUXDEPLOY_SHA256"
fetch_tool linuxdeploy-plugin-qt "$PLUGIN_QT_TAG" linuxdeploy-plugin-qt-x86_64.AppImage \
    "$PLUGIN_QT_SHA256"

rm -rf "$appdir"
mkdir -p "$appdir/usr/bin/recipes" "$appdir/usr/share/licenses/fritchii"
# Like the Windows release (stage_release): no nx2_switches.txt beside the executable, the
# settings are code defaults; the additional terms go with the program.
cp LICENSES/Fritchii-Additional-Terms.txt "$appdir/usr/share/licenses/fritchii/"
# Test packages only: TEST_SWITCHES names a NAME=VALUE file that goes next to the exe as
# nx2_switches.txt (the Windows test packages ship one too). Releases leave it unset.
if [ -n "${TEST_SWITCHES:-}" ]; then
    cp "$TEST_SWITCHES" "$appdir/usr/bin/nx2_switches.txt"
    echo "Test switches: $(grep -c = "$TEST_SWITCHES") from $TEST_SWITCHES"
fi

if [ -z "${RECIPES_ZIP:-}" ]; then
    RECIPES_ZIP="$TOOLS_DIR/Fritchii-v$RECIPES_VERSION-Windows-x64.zip"
    if [ ! -s "$RECIPES_ZIP" ]; then
        if command -v gh > /dev/null && [ -n "${GH_TOKEN:-}" ]; then
            gh release download "fritchii-v$RECIPES_VERSION" --repo Frizti/Fritchii \
                --pattern "Fritchii-v$RECIPES_VERSION-Windows-x64.zip" --dir "$TOOLS_DIR" ||
                rm -f "$RECIPES_ZIP"
        else
            curl -fsSL -o "$RECIPES_ZIP" \
                "https://github.com/Frizti/Fritchii/releases/download/fritchii-v$RECIPES_VERSION/Fritchii-v$RECIPES_VERSION-Windows-x64.zip" ||
                rm -f "$RECIPES_ZIP"
        fi
    fi
fi
# The zip holds Fritchii-vN/recipes/ with the recipe and its own license (RECIPE-LICENSE.txt).
# nx2_precompile.cpp reads <exe dir>/recipes.
if [ -s "${RECIPES_ZIP:-}" ] && unzip -j -o "$RECIPES_ZIP" '*/recipes/*' -d "$appdir/usr/bin/recipes"; then
    echo "Recipes from $(basename "$RECIPES_ZIP"):"
    ls -la "$appdir/usr/bin/recipes"
elif [ "${ALLOW_NO_RECIPES:-0}" = 1 ]; then
    echo "::warning::No shader recipes: this package has no shader preparation"
else
    echo "::error::No shader recipes (set RECIPES_ZIP, or ALLOW_NO_RECIPES=1)"
    exit 1
fi

desktop="$(dirname "$BUILD_DIR")/io.github.frizti.fritchii.desktop"
sed -e 's/^Name=.*/Name=Fritchii/' \
    -e 's/^Comment=.*/Comment=Nintendo Switch emulator tuned for OoT/' \
    -e 's/^Icon=.*/Icon=fritchii/' \
    -e 's/^Keywords=.*/Keywords=Nintendo;Switch;Fritchii;/' \
    dist/io.github.zortos293.lichii.desktop > "$desktop"
icon="$(dirname "$BUILD_DIR")/fritchii.png"
cp dist/fritchii.png "$icon"

# Wayland platform plugins under the names this Qt uses (Qt 6.8: libqwayland-egl.so and
# libqwayland-generic.so; newer Qt: libqwayland.so).
platforms=()
for plugin in libqwayland.so libqwayland-egl.so libqwayland-generic.so; do
    [ -f "$QT_ROOT_DIR/plugins/platforms/$plugin" ] && platforms+=("$plugin")
done
EXTRA_PLATFORM_PLUGINS=$(IFS=';'; echo "${platforms[*]}")
export EXTRA_PLATFORM_PLUGINS
# The Fritchii UI's icons are SVG (src/yuzu/fritchii/icons): Qt loads them through the svg
# imageformat/iconengine plugins, which linuxdeploy only deploys with the svg module named.
# Without them every icon button is blank (Linux test 1, 2026-11-16).
extra_modules=(svg)
if compgen -G "$QT_ROOT_DIR/lib/libQt6WaylandCompositor.so*" > /dev/null; then
    extra_modules+=(waylandcompositor)
fi
EXTRA_QT_MODULES=$(IFS=';'; echo "${extra_modules[*]}")
export EXTRA_QT_MODULES

export PATH="$TOOLS_DIR:$PATH"
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$QT_ROOT_DIR/bin/qmake"
export LD_LIBRARY_PATH="$QT_ROOT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# linuxdeploy's own strip cannot read the RELR sections of Ubuntu 24.04's libraries.
export NO_STRIP=1
export LDAI_VERSION="v$VERSION-$(git rev-parse --short=7 HEAD 2> /dev/null || echo local)"
export LDAI_OUTPUT="$OUT_DIR/$name.AppImage"
export OUTPUT="$LDAI_OUTPUT"
export ARCH=x86_64
# The .zsync is published beside the AppImage on every release; "latest" makes update tools follow
# the newest release.
UPDATE_INFORMATION=${UPDATE_INFORMATION-gh-releases-zsync|Frizti|Fritchii|latest|Fritchii-v*-Linux-x86_64.AppImage.zsync}
if [ -n "$UPDATE_INFORMATION" ]; then
    export UPDATE_INFORMATION LDAI_UPDATE_INFORMATION="$UPDATE_INFORMATION"
else
    unset UPDATE_INFORMATION LDAI_UPDATE_INFORMATION
fi

strip "$BUILD_DIR/bin/$exe"

# The host's GPU driver loads into this process and may need a newer C++ runtime than the build
# system's; every system new enough for this glibc has one at least as new, so the host's is used.
# The Vulkan loader likewise has to be the one that goes with the host's driver.
# These are left to the host by linuxdeploy, but minimal desktop installs can lack them.
extra_libs=()
for lib in libusb-1.0.so.0 libOpenGL.so.0 libfribidi.so.0 libcom_err.so.2 libgpg-error.so.0; do
    [ -f "/usr/lib/x86_64-linux-gnu/$lib" ] && extra_libs+=(--library "/usr/lib/x86_64-linux-gnu/$lib")
done

linuxdeploy-x86_64.AppImage --appdir "$appdir" \
    --executable "$BUILD_DIR/bin/$exe" \
    --desktop-file "$desktop" \
    --icon-file "$icon" \
    --exclude-library 'libstdc++.so*' \
    --exclude-library 'libgcc_s.so*' \
    --exclude-library 'libvulkan.so*' \
    "${extra_libs[@]}" \
    --plugin qt \
    --output appimage

test -s "$LDAI_OUTPUT"
test -f "$appdir/usr/bin/qt.conf"
# appimagetool writes the .zsync itself when it finds zsyncmake; otherwise make it here.
if [ -n "${UPDATE_INFORMATION:-}" ] && [ ! -s "$LDAI_OUTPUT.zsync" ]; then
    if command -v zsyncmake > /dev/null; then
        zsyncmake -u "$(basename "$LDAI_OUTPUT")" -o "$LDAI_OUTPUT.zsync" "$LDAI_OUTPUT"
    else
        echo "::warning::No zsyncmake (apt package zsync): no .zsync, update tools can't update this AppImage"
    fi
fi
for plugin in imageformats/libqsvg.so iconengines/libqsvgicon.so; do
    test -f "$appdir/usr/plugins/$plugin" ||
        { echo "::error::No $plugin in the AppDir: the UI icons would be blank"; exit 1; }
done

# The tar.gz is the same AppDir, for players who can't run AppImages (no FUSE, sandboxes).
# Its own folder: next to BUILD_DIR there may be a source checkout named fritchii (WSL builds,
# 2026-11-16: the old "$(dirname "$BUILD_DIR")/fritchii" deleted it).
tar_root="$(dirname "$BUILD_DIR")/fritchii-package"
tar_dir="$tar_root/fritchii"
rm -rf "$tar_root"
mkdir -p "$tar_root"
cp -a "$appdir" "$tar_dir"
ln -s "usr/bin/$exe" "$tar_dir/$exe"
tar -czf "$OUT_DIR/$name.tar.gz" -C "$tar_root" fritchii

# The host's Vulkan loader and C++ runtime must be the ones used (see above).
if tar -tzf "$OUT_DIR/$name.tar.gz" | grep -E '/(libvulkan|libstdc\+\+|libgcc_s)\.so'; then
    echo "::error::The package bundles the Vulkan loader or the C++ runtime"
    exit 1
fi
# Checksums for players who check by hand: "sha256sum -c <file>.sha256" in the download folder.
for package in "$LDAI_OUTPUT" "$OUT_DIR/$name.tar.gz"; do
    (cd "$(dirname "$package")" && sha256sum "$(basename "$package")" > "$(basename "$package").sha256")
done
ls -la "$LDAI_OUTPUT"* "$OUT_DIR/$name.tar.gz"*
