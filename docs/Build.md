# Building Fritchii

> [!WARNING]
> This guide is for developers. Players should download a release instead.

Fritchii is built and released for Windows x64.

## What you need

- Visual Studio 2022 (or its Build Tools) with the C++ workload
- clang-cl (LLVM) is what releases use; MSVC also builds
- CMake and [Ninja](https://ninja-build.org/) in your PATH
- Qt 6 for MSVC 2022 x64 (point `CMAKE_PREFIX_PATH` at it)
- Git (dependencies are fetched by CMake on the first configure)

## Configure and build

Load the Visual C++ environment first: `tools/windows/load-msvc-env.ps1` (PowerShell) or
`tools/windows/load-msvc-env.sh` (Git Bash / MSYS2). Then, from the source folder:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
    -DCMAKE_PREFIX_PATH=<path to Qt>/msvc2022_64 \
    -DYUZU_TESTS=OFF -DYUZU_USE_BUNDLED_QT=OFF
cmake --build build --target yuzu
```

The executable is `build/bin/lichii.exe` (the target is still called `yuzu`; both get
renamed later). See the root `CMakeLists.txt` for the other options.

## AV1 hardware decoding

The bundled Windows x64 FFmpeg provides AV1 through libdav1d. To use AV1 hardware
decoding in a custom build, set `-DFFmpeg_CUSTOM_DIR=<path to an FFmpeg package>`
to a compatible package that includes the native AV1 decoder, a supported AV1 hardware
accelerator, and libdav1d. Select GPU for NVDEC emulation in Fritchii. Software decoding
remains available for the bundled package and for streams that fail hardware startup.

## If the configure fails

- Delete `<build folder>/CMakeCache.txt` and the CPM cache (`.cache/cpm`), then configure again.
- Read the first error CMake prints; most failures are a missing tool or a wrong Qt path.

See [Caveats](Caveats.md) for platform quirks.
