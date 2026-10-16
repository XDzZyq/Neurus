# Neurus

A real-time renderer built on C++20 and Vulkan-HPP 1.4, for experimenting with
modern rendering techniques inside a strictly layered codebase.

[![CI](https://github.com/XDzZyq/Neurus/actions/workflows/ci.yml/badge.svg)](https://github.com/XDzZyq/Neurus/actions/workflows/ci.yml)
[![Docs](https://github.com/XDzZyq/Neurus/actions/workflows/docs_build.yml/badge.svg)](https://xdzzyq.github.io/Neurus/)

Neurus is as much an editor as a renderer: a Qt6 desktop application with a
dockable viewport, scene outliner, transform gizmos and a live GLSL editor,
sitting on a deferred PBR pipeline. The layers are kept deliberately isolated -
the renderer never mutates application state, the UI never sees Vulkan - so
passes and panels can be replaced without disturbing anything else.

## Documentation

**<https://xdzzyq.github.io/Neurus/>**

Architecture and data flow, renderer internals, editor and UI guides, the build
and test manuals, a Doxygen API reference generated from `src/`, and an
interactive map of the source tree.

## What's inside

- **Deferred PBR pipeline** - shadow mapping, SSAO, image-based lighting, FXAA,
  and temporal accumulation via Halton jitter
- **Qt6 editor** - dockable viewport (Qt Advanced Docking), outliner, property
  panels, transform gizmo, debug overlays, and translated UI
- **Live shader editing** - per-mesh GLSL with pipeline rebuild on save
- **Project workflow** - undo/redo operations and project serialization
- **Tested** - GPU and non-GPU suites, including reference-image regression

## Getting started

Requires CMake 3.27+, the Vulkan SDK 1.4.x, Qt 6, and a C++20 toolchain
(Visual Studio 2022 or AppleClang). GNU Make is used as a thin wrapper.

```bash
git clone --recurse-submodules https://github.com/XDzZyq/Neurus.git
cd Neurus

make update    # init submodules, fetch pre-compiled deps, configure CMake
make build     # Debug build
make check     # run the test suite
```

`make help` lists every target. Platform specifics, the two build trees, and the
pre-compiled dependency system are covered in the
[build guide](docs/build/build.md).

## Project structure

```
src/
  app/        application lifetime and wiring
  render/     renderer: passes, pipelines, GPU resources (Vulkan-HPP)
  editor/     application logic: scene mutation, controllers, operations
  ui/         Qt6 Widgets presentation
  scene/      Vulkan-free scene objects (camera, light, mesh, transform)
  asset/      Vulkan-free CPU-side asset loading
  core/       UIDs, serialization, logging, render config
  platform/   native window and surface glue
test/         mirrors src/, plus shared GPU test fixtures
res/          shaders, meshes, textures, UI assets, translations
docs/         product documentation (published as the site above)
cmake/        CMake helper modules
lib/          Neurus-Lib submodule: vendored sources and prebuilt binaries
scripts/      tooling: translation extraction, knowledge-graph cleanup
```

## Contributing

[`AGENTS.md`](AGENTS.md) is the entry point for the hard rules - layer
isolation, Vulkan ownership, the coordinate system - and routes to the coding
conventions and development workflow under
[`.github/instructions/`](.github/instructions/README.md).

## License

[GNU GPL-3.0](LICENSE)
