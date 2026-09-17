# Neurus

**C++20 Vulkan-HPP 1.4 Real-time Renderer**

A real-time rendering framework with strict four-layer architecture
(Renderer ↔ Editor ↔ UI ↔ Data & Resource). Built for experimentation
with modern rendering algorithms.

[![CI](https://github.com/XDzzzzzZyq/Neurus/actions/workflows/ci.yml/badge.svg)](https://github.com/XDzzzzzZyq/Neurus/actions/workflows/ci.yml)

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│ UI Layer (Qt6 Widgets + ADS)                                 │
│  owns: QWindow, UIEvents (QObject singleton)                 │
└─────────────────────┬────────────────────────────────────────┘
                      │ Qt Signals/Slots
                      ▼
┌──────────────────────────────────────────────────────────────┐
│ Editor Layer                                                 │
│  owns: Project, UploadManager, Controllers                   │
└─────────────────────┬────────────────────────────────────────┘
                      │
            ┌─────────┴──────────┐
            ▼                    ▼
┌───────────────────┐  ┌────────────────────────────────────────┐
│ Data & Resource   │  │ Renderer Layer (Vulkan-HPP vk::raii)   │
│  owns: allocators │  │  owns: VkInstance, VkDevice, VkQueue,  │
│  descriptor pools │  │   VkSwapchainKHR, VkPipeline,          │
│  pipeline cache   │  │   VkCommandBuffer, all GPU resources   │
└───────────────────┘  └────────────────────────────────────────┘
```

## Prerequisites

- **Visual Studio 2022** (MSVC C++20 toolchain)
- **CMake** >= 3.27
- **Vulkan SDK** 1.4.x ([LunarG Vulkan SDK](https://vulkan.lunarg.com/))
  - Set `VULKAN_SDK` environment variable
- **Qt 6.8+** ([Qt for Windows](https://www.qt.io/download))
  - Set `CMAKE_PREFIX_PATH` to Qt install (e.g., `C:\Qt\6.8.0\msvc2022_64`)
- **GNU Make** (via [MSYS2](https://www.msys2.org/), [Chocolatey](https://chocolatey.org/), or Git Bash)

## Quick Start

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/XDzzzzzZyq/Neurus.git
cd Neurus

# Download pre-compiled third-party libraries + configure (recommended)
make update

# Build Debug
make build

# Or only build the app or the test
make app
make test

# Run the application
./build/debug/Neurus.exe

# Build Release
cmake --preset release
cmake --build build/release

# Generate Visual Studio 2022 solution
make nobuild
# Opens: ../Neurus_VS2022/Neurus.sln

# Run tests
make check
```

## Project Structure

```
Neurus/
├── .github/
│   ├── instructions/       # Architecture/component docs
│   └── workflows/          # CI configuration
├── cmake/                  # CMake helper modules (Dependencies.cmake)
├── dep/                    # Git submodule dependencies (source fallback)
├── lib/                    # Pre-compiled third-party libraries (git-ignored)
├── res/shaders/            # GLSL shader source files
├── scripts/                # Utility scripts (setup_dependencies.py)
├── src/
│   ├── render/             # Renderer layer (Vulkan-HPP)
│   ├── editor/             # Editor layer (logic, controllers)
│   │   └── controllers/    # Controller implementations (CameraController)
│   ├── ui/                 # UI layer (Qt6 Widgets + ADS)
│   │   └── qml/            # QML source files
│   ├── data/               # Data & Resource layer
│   └── main.cpp            # Application entry point
├── test/
│   ├── render/             # Renderer unit tests
│   └── editor/             # Editor unit tests
├── AGENTS.md               # AI agent instructions
├── CMakeLists.txt          # Root CMake build
├── CMakePresets.json       # CMake presets
├── Makefile                # Convenience build wrapper
└── README.md               # This file
```

## Current Scope (Deferred PBR MVP)

The current deliverable is a deferred PBR renderer with shadow mapping,
screen-space ambient occlusion, image-based lighting, and full G-Buffer pipeline:

- Vulkan-HPP RAII instance, device, swapchain, pipeline
- `VK_KHR_dynamic_rendering` for render passes
- Qt6 Widgets window with Qt-Advanced-Docking-System (ADS)
- Viewport as dockable central widget via ADS `CenterDockWidgetArea`
- Qt Signals/Slots UIEvents singleton (UI↔Editor)
- Typed EventQueue for Editor↔Renderer event dispatch
- Swapchain recreation on window resize
- Validation layers in Debug builds
- Embedded SPIR-V shaders (compiled at CMake time)
- Non-GPU Google Test samples (UIEvents, EventQueue)
- Event-driven CameraController (orbit/zoom/dolly/pan via EventQueue)
- OBJ mesh loading with MeshData (icosphere, cube, etc.)
- Deferred PBR pipeline: ShadowDepthPass → GeometryPass → SSAOPass → LightingPass → IBLPass
- Temporal shadow accumulation via Halton jitter (fixed EMA / moving average)
- Shader Editor panel: per-mesh GLSL editing (Code + Structure modes) with live pipeline rebuild
- Reference-image regression tests (capture → compare PNG)

## Dependency Management

Third-party libraries that benefit from ahead-of-time compilation (shaderc,
Qt Advanced Docking) can be used as pre-compiled binaries via
[Neurus-Lib](https://github.com/XDzzzzzZyq/Neurus-Lib).
Run `make update` to download the latest binaries into `lib/<platform>/`.
The CMake build system prefers pre-compiled binaries and falls back to
source builds in `dep/` automatically. Qt Advanced Docking carries an extra
condition: because it shares Qt's inline container template instantiations with
our own translation units, its archive is only linked when the
`build_info.txt` stamp beside it records the same Qt version and C++ standard
this build uses — otherwise CMake says why and builds it from source. See
[build instructions](.github/instructions/build.instructions.md) for details.

## Code Style

Follows [Blender C/C++ guidelines](https://developer.blender.org/docs/handbook/guidelines/c_cpp/):

- **Indentation**: Tabs
- **Braces**: Allman style
- **Naming**: PascalCase for types and methods
- **Comments**: Doxygen-style `/** @brief ... */` on public APIs
- **RAII**: All resources initialized in constructor, released in destructor
- **Includes**: Local first, then third-party, then STL; use `#pragma once`

## License

[MIT](LICENSE)
