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
