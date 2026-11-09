# Building and running Fritchii on Linux

Tested on CachyOS (build) and SteamOS (play: Ryzen 5 5500, RX 6900 XT, RADV). Arch-based commands below; other distributions need the same packages under their names.

## Packages

```sh
sudo pacman -Syu --needed base-devel clang lld llvm cmake ninja git \
    boost catch2 enet ffmpeg fmt glslang libzip lz4 nlohmann-json openssl opus zlib zstd zip unzip \
    qt6-base qt6-multimedia qt6-charts qt6-5compat qt6-svg qt6-wayland quazip-qt6 \
    sdl3 libusb spirv-tools spirv-headers vulkan-headers vulkan-utility-libraries
# The machine that plays (AMD): the RADV Vulkan driver.
sudo pacman -S --needed vulkan-radeon mesa
```

## Configure and build

A Release build with clang, lld, ThinLTO and the shipped PGO profile (`eglantine.profdata`, the same one the release workflow uses):

```sh
cmake -S . -B build/release -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld -Wl,--thinlto-cache-dir=$PWD/build/release/thinlto-cache" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" \
    -DCMAKE_CXX_FLAGS="-fprofile-use=$PWD/eglantine.profdata -Wno-backend-plugin -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -gline-tables-only" \
    -DENABLE_LTO=ON \
    -DYUZU_BUILD_PRESET=zen2 \
    -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF -DYUZU_CMD=ON -DYUZU_ROOM=OFF \
    -DYUZU_USE_BUNDLED_QT=OFF \
    -DVulkanHeaders_FORCE_BUNDLED=ON -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
    -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF -DENABLE_WERROR=OFF
cmake --build build/release
```

- `YUZU_BUILD_PRESET=zen2` targets Zen 2 and newer (Zen 3 included). Leave it out for a build that runs on any x86-64.
- `-gline-tables-only` adds line tables only, so `EDEN_NX2_SAMPLE_PROFILE` samples resolve to functions and lines (`tools/nx2/sample_report.py`), at no cost to speed.
- The ThinLTO cache makes a rebuild after a small change relink only what changed.
- The output is `build/release/bin/lichii` with `nx2_switches.txt` next to it. Each build copies `dist/nx2_switches.txt` over that file.
- To check what CI checks, also build with GCC 13 and `-DYUZU_TESTS=ON`, as `.github/workflows/build-check.yml` does.

## Running

- **Copy to the machine that plays:** `lichii`, `nx2_switches.txt` and, for shader preparation, `recipes/*.nx2recipe`, all into one folder.
  - `nx2_switches.txt` is read from the executable's folder (`/proc/self/exe`).
  - `~/.config/eden/nx2_switches.txt` overrides entries from it.
- **Shader preparation:**
  - It runs on RADV, and RADV keeps the compiled pipelines in its own cache. Give that cache room: `MESA_SHADER_CACHE_MAX_SIZE=10G`.
  - The first launch of a new release prepares again (~120,000 pipelines for Ocarina of Time). Let it finish before measuring performance.
- **Thread pinning:**
  - The GPU threads and guest cores are pinned from the sysfs CPU topology.
  - *Exclusive GPU cores* (Graphics > Advanced) also keeps other threads off the GPU threads' cores.
- **Frame generation under gamescope:** presents go out one per refresh on the refresh grid, so generated frames aren't dropped.
- **HDMI audio on AMD:** keep the card profile on surround (`output:hdmi-surround-extra3` on the test TV). Switching the profile to stereo made all audio cut in and out.
- **Logs:** `~/.local/share/eden/log/eden_log.txt`. `EDEN_NX2_GPU_PROFILE=2` in `nx2_switches.txt` adds per-thread zone times every 10 s.
