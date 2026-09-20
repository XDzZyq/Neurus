# Build & Test

## Prerequisites

- Visual Studio 2022 (MSVC C++20 toolchain)
- CMake >= 3.27
- Vulkan SDK 1.4.x (with `$env:VULKAN_SDK` set)
- Qt 6.8+ (with `CMAKE_PREFIX_PATH` pointing to install)
- GNU Make (via MSYS2, Chocolatey, or Git Bash)

## Quick Start

```
# Clone with submodules
git clone --recurse-submodules https://github.com/XDzzzzzZyq/Neurus.git
cd Neurus

# Init submodules, download pre-compiled deps, configure CMake
make update

# Build debug
cmake --build build --config Debug

# Build release
cmake --preset win && cmake --build build --config Release

# Generate VS 2022 solution (outside source tree)
make nobuild
# Opens: ../Neurus_VS2022/Neurus.sln

# Build and run tests
make check
```

## Dependency System

Neurus supports two modes for third-party dependencies that can be compiled ahead of time:

### Pre-compiled (recommended)

Run `make update` to download pre-compiled binaries from the
[Neurus-Lib](https://github.com/XDzzzzzZyq/Neurus-Lib) GitHub Release.
Binaries are extracted into `lib/<platform>/` (git-ignored).

The CMake build system automatically detects pre-compiled libraries and skips
source builds for:

- **shaderc** — shader compilation library (saves ~40-60s per build)
- **qtadvanceddocking** — Qt Advanced Docking System (saves ~15s per build),
  **only when its compatibility stamp matches this build** (see below)

### qtadvanceddocking must match the Qt version exactly

ADS is a Qt C++ library, so its objects carry coalesced (`linkonce_odr`)
instantiations of Qt's inline container templates — the same symbols our own
translation units emit. The linker keeps one definition of each for the whole
binary, so an ADS archive is only safe to link when it was compiled against the
*identical* Qt headers and C++ standard.

Ignoring this cost a day of debugging: an archive built against Homebrew Qt
6.11.1 linked into TUs compiled against CI's Qt 6.11.2 crashed with `SIGSEGV at
0x1`, on the Debug leg only. Qt 6.11.2 had rewritten
`QtPrivate::QPodArrayOps<T>` without changing its mangled names, and `-O3`
hides the mismatch by inlining per TU.

So every pre-compiled ADS archive ships a `build_info.txt` stamp beside it:

```
qt_version=6.11.2
cxx_standard=20
ads_version=4.5.0
compiler=AppleClang 17.0.0.17000013
```

`cmake/Dependencies.cmake` parses it (never `include()`s it) and uses the
archive only when `qt_version` and `cxx_standard` match this build exactly.
Otherwise it says why and builds ADS from source instead — a slower build rather
than a runtime crash. A missing Debug archive is also a mismatch: the Release
one is never substituted for it.

Consequences for maintainers:

- **Rebuild the archives whenever Qt changes.** `brew upgrade qt` alone will
  silently move the build onto the source fallback.
  `scripts/build_macos_libs.sh --ads-only` rebuilds just ADS and re-stamps it.
- The archives are produced through `cmake/ads_standalone/CMakeLists.txt`, not
  by configuring `dep/qtadvanceddocking` directly, because the submodule
  hardcodes `CXX_STANDARD 17` for Qt6 while Neurus is C++20 — another ODR axis,
  since Qt's containers branch on `#if __cplusplus >= 202002L`. The wrapper pins
  the standard and writes the stamp.
- `-DNEURUS_ADS_FROM_SOURCE=ON` forces the source build regardless.
- shaderc needs none of this: it exposes a C API and links no Qt, so its binary
  does not depend on the headers we compile against.

### Source build (fallback)

If `lib/<platform>/` does not contain pre-compiled binaries, CMake falls
back to building from source in `dep/` (the existing behavior). This
requires a one-time shaderc dependency sync:

```
cd dep/shaderc
python utils/git-sync-deps
```

## MSVC: `/bigobj` is mandatory

`CMakeLists.txt` passes `/bigobj` to every MSVC compile. This is not optional
polish — without it the cereal registration TUs fail to compile with:

```
error C1128: 节数超过对象文件格式限制: 请使用 /bigobj 进行编译
   (number of sections exceeded object file format limit: compile with /bigobj)
```

Each `CEREAL_REGISTER_TYPE(...)` expands to template specializations, and MSVC
puts every instantiated function in its own COMDAT section. The registration
files (`src/editor/operations/registrations/OperationRegistration.cpp`,
`src/scene/registrations/TypeRegistration.cpp`,
`src/asset/registrations/DataRegistration.cpp`) are pure macro lists with no
code to merge, so their section count climbs with every registered type and
eventually crosses the default COFF limit of 65535. Adding a handful of new
Operations is enough to trip it — which is exactly what happened when the debug
object property ops landed.

`/bigobj` only widens the section-count limit; it changes no codegen and no
ABI. `cereal` builds itself with the same flag (`dep/cereal/CMakeLists.txt`).
If a future TU hits C1128 again, the flag is already there — the answer is to
split that TU, not to add the flag.

## CI

- See `.github/workflows/ci.yml` for the exact matrix and steps.
- CI runs Windows x64 and macOS arm64. GPU tests are excluded from CI.
- CI attempts to fetch pre-compiled dependencies first; falls back to source build on failure.

## Testing

- Framework: Google Test
- Non-GPU tests run in CI (UIEvents, EventQueue, EditorContext)
- GPU tests require a Vulkan 1.4-capable device
- Run all tests: `make check` (or `cd build && ctest -C Debug --output-on-failure`)
- Run specific tests: `make check FILTER="-R DeferredShading"`
- Build only the test binary: `make build test` (or `make test`)
- On local machine, launch `Neurus.exe` to check terminal output and runtime errors.
- See `.github/instructions/test.instructions.md` for full testing standards and patterns.

## Lint / Format

- No repo-wide formatter configured.
- Follow Blender C/C++ style guidelines (see `.github/instructions/style.instructions.md`).
- Do not run clang-format on project code unless explicitly requested.
