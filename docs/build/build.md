# Build

Neurus builds with CMake presets on Windows (MSVC) and macOS (Ninja), with
pre-compiled dependencies fetched by the `lib/` submodule.

## Prerequisites

- Visual Studio 2022 (MSVC C++20 toolchain)
- CMake >= 3.27
- Vulkan SDK 1.4.x (with `$env:VULKAN_SDK` set)
- Qt 6.8+ (with `CMAKE_PREFIX_PATH` pointing to install)
- GNU Make (via MSYS2, Chocolatey, or Git Bash)

## Quick Start

```
# Clone with submodules
git clone --recurse-submodules https://github.com/XDzZyq/Neurus.git
cd Neurus

# Init submodules, download pre-compiled deps via lib/, configure CMake
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

## CMake Presets

`CMakePresets.json` defines three configure presets and their build presets:

- `win` - Windows x64 (Visual Studio 17 2022, MSVC), binaryDir `build/`, Debug +
  Release via `--config`; sets `CMAKE_PREFIX_PATH`, `NEURUS_BUILD_TESTS=ON`,
  `NEURUS_ENABLE_VALIDATION=ON`. Build presets: `win-debug`, `win-release`.
- `vs2022` - Visual Studio 2022 solution for IDE development, generated at
  `../Neurus_VS2022`.
- `macos` - macOS arm64 Ninja Multi-Config, binaryDir `build/`, Debug + Release
  in one tree. Build presets: `macos`, `macos-release`.

Makefile convenience targets:

- `make update` - init submodules, download pre-compiled deps, configure CMake
- `make build` - full Debug build; `make build app` / `make build test` narrow to
  `Neurus.exe` / `neurus_test`
- `make check` - run all tests (or `make check FILTER="-R Pattern"`)
- `make release` - configure + build Release
- `make nobuild` - generate the VS 2022 solution at `../Neurus_VS2022`
- `make clean` - clean the Debug build

## Dependency System

Neurus supports two modes for third-party dependencies that can be compiled
ahead of time.

**Pre-compiled (recommended).** Run `make update` to init the submodules and
download pre-compiled binaries from the
[Neurus-Lib](https://github.com/XDzZyq/Neurus-Lib) GitHub Release. Binaries are
extracted into `lib/prebuilt/<platform>/`, inside the `lib` submodule
(git-ignored there). The CMake build system automatically detects pre-compiled
libraries and skips source builds for:

- **shaderc** - shader compilation library (saves ~40-60s per build)
- **qtadvanceddocking** - Qt Advanced Docking System (saves ~15s per build),
  **only when its compatibility stamp matches this build** (see below)

**Source build (fallback).** If `lib/prebuilt/<platform>/` does not contain
pre-compiled binaries, CMake falls back to building from source in `lib/src/`.
This requires a one-time shaderc dependency sync:

```
cd lib/src/shaderc
python utils/git-sync-deps
```

## Python Environment (Scripting)

The C++ scripting layer (`src/script/`) embeds CPython and uses pybind11 as a
build-time dependency. `make update` provisions a `uv`-managed virtual
environment at `lib/.venv` (inside the `lib` submodule, git-ignored there):

- The interpreter is pinned in `lib/.python-version`.
- Requirements live in `lib/environment.txt` (currently pybind11).
- `lib/scripts/setup_python_env.ps1` (Windows) / `setup_python_env.sh`
  (macOS/Linux) create the environment; the step is idempotent and prints
  `Using uv Python env at lib/.venv` on success.

`uv` must be installed and on `PATH`. CMake links the base CPython via
`find_package(Python COMPONENTS Development.Embed)`; the environment supplies
pybind11. Set `NEURUS_PYTHON_ENV` to override the environment path at runtime.

## qtadvanceddocking Must Match the Qt Version Exactly

ADS is a Qt C++ library, so its objects carry coalesced (`linkonce_odr`)
instantiations of Qt's inline container templates - the same symbols our own
translation units emit. The linker keeps one definition of each for the whole
binary, so an ADS archive is only safe to link when it was compiled against the
*identical* Qt headers and C++ standard.

Ignoring this cost a day of debugging: an archive built against Homebrew Qt
6.11.1 linked into TUs compiled against CI's Qt 6.11.2 crashed with `SIGSEGV at
0x1`, on the Debug leg only. Qt 6.11.2 had rewritten
`QtPrivate::QPodArrayOps<T>` without changing its mangled names, and `-O3` hides
the mismatch by inlining per TU.

So every pre-compiled ADS archive ships a `build_info.txt` stamp beside it:

```
qt_version=6.11.2
cxx_standard=20
ads_version=4.5.0
compiler=AppleClang 17.0.0.17000013
```

`cmake/Dependencies.cmake` parses it (never `include()`s it) and uses the
archive only when `qt_version` and `cxx_standard` match this build exactly.
Otherwise it says why and builds ADS from source instead - a slower build rather
than a runtime crash. A missing Debug archive is also a mismatch: the Release
one is never substituted for it.

Consequences for maintainers:

- **Rebuild the archives whenever Qt changes.** `brew upgrade qt` alone will
  silently move the build onto the source fallback.
  `lib/scripts/build_prebuilt_macos.sh --ads-only` rebuilds just ADS and
  re-stamps it.
- The archives are produced through `lib/scripts/ads_standalone/CMakeLists.txt`,
  not by configuring `lib/src/qtadvanceddocking` directly, because the submodule
  hardcodes `CXX_STANDARD 17` for Qt6 while Neurus is C++20 - another ODR axis,
  since Qt's containers branch on `#if __cplusplus >= 202002L`. The wrapper pins
  the standard and writes the stamp.
- `-DNEURUS_ADS_FROM_SOURCE=ON` forces the source build regardless.
- shaderc needs none of this: it exposes a C API and links no Qt, so its binary
  does not depend on the headers we compile against.

## MSVC: `/bigobj` Is Mandatory

`CMakeLists.txt` passes `/bigobj` to every MSVC compile. This is not optional
polish - without it the cereal registration TUs fail to compile with:

```
error C1128: number of sections exceeded object file format limit: compile with /bigobj
```

Each `CEREAL_REGISTER_TYPE(...)` expands to template specializations, and MSVC
puts every instantiated function in its own COMDAT section. The registration
files (`src/editor/operations/registrations/OperationRegistration.cpp`,
`src/scene/registrations/TypeRegistration.cpp`,
`src/asset/registrations/DataRegistration.cpp`) are pure macro lists with no code
to merge, so their section count climbs with every registered type and
eventually crosses the default COFF limit of 65535. Adding a handful of new
Operations is enough to trip it - which is exactly what happened when the debug
object property ops landed.

`/bigobj` only widens the section-count limit; it changes no codegen and no ABI.
`cereal` builds itself with the same flag (`lib/src/cereal/CMakeLists.txt`). If a
future TU hits C1128 again, the flag is already there - the answer is to split
that TU, not to add the flag.

## Two Build Trees Exist, and Only One Is Live

CI configures `build/` as a **Ninja Multi-Config** tree. The tree local work
actually builds is the **single-config** `build/debug`
(`CMAKE_BUILD_TYPE=Debug`). They are separate trees with separate test
discovery, so a command aimed at the wrong one does not fail, it answers about a
different codebase:

| tree | generator | run its tests with | binaries |
|------|-----------|--------------------|----------|
| `build/debug` | `Ninja` (single-config) | `cd build/debug && ctest` | `build/debug/Neurus`, `build/debug/neurus_test` |
| `build/` | `Ninja Multi-Config` | `cd build && ctest -C Debug` | `build/Debug/...` per config |

**A bare `ctest` in `build/` prints `Total Tests: 0` and exits 0.** A multi-config
tree has no tests without `-C`, so that run is a green that executed nothing -
and a stale `-C Debug` run is worse, because it reports a plausible pass count
from whatever its last configure saw. Always verify from `build/debug`, and read
an unexpected suite total as "wrong tree", not as tests that disappeared.

macOS filesystems are case-insensitive by default, so the Multi-Config tree's
Debug output directory `build/Debug` **is** `build/debug`, the single-config
tree's root. Re-running `cmake --preset macos` and building Debug writes into the
live tree. Pick one tree per machine and stay in it.

**Test working directory**: `test/CMakeLists.txt` pins
`gtest_discover_tests(WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")`, so every test
runs with its CWD at its own tree's *root* - where the POST_BUILD step copies
`res/`. For `build/debug` the root and the binary are the same directory; for a
multi-config tree the binary is one level down (`build/Debug/neurus_test` on
CI), which is why CI runs `ctest` from `build/`. Launching the binary by hand
from anywhere else makes `res/` unresolvable and can write reference images to a
path outside the tree entirely.

## CI

- See `.github/workflows/ci.yml` for the exact matrix and steps.
- CI runs Windows x64 and macOS arm64. GPU tests are excluded from CI.
- CI attempts to fetch pre-compiled dependencies first; falls back to source
  build on failure.

## Lint and Format

- No repo-wide formatter configured.
- Follow Blender C/C++ style guidelines (see [Style](../style/style.md)).
- Do not run clang-format on project code unless explicitly requested.
