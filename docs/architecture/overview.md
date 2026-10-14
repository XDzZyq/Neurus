# Architecture Overview

Neurus is a C++20 Vulkan-HPP 1.4 real-time renderer designed for experimentation
with modern rendering algorithms, organized into strictly isolated layers.

## System Design Philosophy

The architecture prioritizes:

- **Strict layer isolation** - Renderer <-> Editor <-> UI <-> Asset boundaries
  must not be violated
- **Minimal global state** - State is explicit and localized
- **Explicit data flow** - Communication via UIEvents (Qt Signals/Slots),
  EventQueue (typed event dispatcher), and Context objects
- **Stateless rendering** - Renderer does not own application-level state
- **Deterministic GPU resource management** - Vulkan resources have explicit
  RAII ownership via the `vk::raii` namespace

## Layer Diagram

```
+--------------------------------------------------------------+
| UI Layer (Qt6 Widgets + ADS)                                 |
|  owns: native window, VkSurfaceKHR, UIEvents (QObject)       |
|  presentation only. No rendering logic.                      |
+----------------------------+---------------------------------+
                             |
                             | Qt Signals/Slots
                             v
+--------------------------------------------------------------+
| Editor Layer                                                 |
|  owns: Scene, RenderConfig, UploadManager, Controllers       |
|  Application logic, scene mutation, event orchestration      |
+----------------------------+---------------------------------+
                             |
                   +---------+----------+
                   v                    v
+----------------------------+  +------------------------------+
| Asset Layer                |  | Renderer Layer (Vulkan-HPP   |
|  Vulkan-free               |  |  vk::raii)                   |
|  MeshData, ImageData,      |  |  owns: VkInstance, VkDevice, |
|  PixelFormat               |  |  VkQueue, VkSwapchainKHR,    |
|                            |  |  VkPipeline,                 |
|                            |  |  VkCommandBuffer, all GPU    |
+----------------------------+  |  resources                   |
| Scene Layer                |  |  MeshGPU, EnvironmentGPU     |
|  Vulkan-free               |  |  (via Cache)                 |
|  Camera, Light, Mesh,      |  |  consumes: VkSurfaceKHR      |
|  Transform                 |  |  (read-only)                 |
+----------------------------+  +------------------------------+
```

Scene objects (`src/scene/`) and Core utilities (`src/core/`) are shared across
layers.

### Generated Structure Graph

<!-- The link below is raw HTML on purpose. graphify.html is generated build
     output and gitignored, so a markdown link would be reported by MkDocs link
     validation and fail `mkdocs build --strict` on any build where the graph
     has not been generated (a plain `mkdocs serve`, or a CI run where the
     non-fatal graphify step failed). Raw hrefs are passed through unvalidated
     AND unrewritten, so this path is URL-relative to /architecture/overview/,
     not source-relative like a markdown link would be. -->

The diagram above is hand-maintained. For a generated counterpart, the
<a href="../../graphify.html" target="_blank">structure graph</a> maps the
first-party symbols in `src/` - classes, namespaces, enums, methods and free
functions - with the `calls`, `contains`, `inherits` and `imports` edges between
them, clustered into communities that track the layer boundaries above. It is
built from tree-sitter ASTs by
[graphify](https://github.com/Graphify-Labs/graphify) during the documentation
build, with no model involved. Generate it locally with `make doc`.

Third-party and language-level nodes are filtered out, so STL containers,
Vulkan-HPP handles, GLM vector types and Qt widgets do not appear as symbols -
only the Neurus code that uses them. Duplicate and file-versus-class nodes are
merged, and communities of three nodes or fewer are pruned, which takes the raw
extraction from roughly 5,300 nodes to 3,000 across 125 communities.

Use it to trace a call path across layers, or to check whether a dependency
crosses a boundary it should not. It complements the
[API Reference](../Neurus/classes.md), which documents individual declarations
rather than the relations between them.

Three caveats. Shaders do not appear, because GLSL is outside graphify's
code-extension set - the Doxygen reference shares that blind spot. The page
loads its layout engine from a CDN, so it will not draw offline. And it is a
single document of roughly 2.7 MB with every node embedded, so expect a slow
first load and sluggish interaction on modest hardware.

## Current Scope

The project delivers a deferred PBR renderer with a geometry pass, lighting
compute pass and full G-Buffer pipeline through the isolated layers.

**Implemented:**

- Vulkan-HPP RAII instance, device, swapchain and pipelines
- `VK_KHR_dynamic_rendering` for render passes
- Qt6 Widgets window with Qt-Advanced-Docking-System (ADS)
- Viewport as a dockable central widget
- `UIEvents` singleton over Qt signals (UI <-> Editor)
- Typed `EventQueue` for Editor <-> Renderer dispatch
- Swapchain recreation on window resize
- Validation layers in Debug builds
- Embedded SPIR-V shaders (compiled at CMake time)
- OBJ mesh loading into `MeshData`
- Deferred pipeline: ShadowDepthPass -> GeometryPass (G-Buffer) -> SSAOPass ->
  Light + ShadowIntensity -> LightingPass -> IBLPass -> SelectionOutlinePass ->
  ComposePass (gamma) -> FXAAPass (conditional) -> DebugPass -> GizmoPass ->
  blit to swapchain
- Centralized image barriers (`Barrier::Transition`, `ImageState`)
- Screenshot capture and PNG readback
- Undo/redo operation system and project serialization
- GPU and CPU profiling (`GPUProfiler`)
- GPU tests with a shared `VulkanTestShared` base class, plus reference-image
  regression tests

**Not yet implemented:**

- Screen-space reflections, ray tracing and mesh shaders
- glTF import and a general asset pipeline (OBJ is supported)
- Threaded rendering and Vulkan Memory Allocator (VMA)

## Future Architecture Evolution

- GPU-driven rendering with indirect draws
- Threaded command-buffer recording
- Vulkan Memory Allocator (VMA) integration
- Asset pipeline (glTF loading, texture compression)
- Screen-space reflections and ray tracing
