# Architecture Overview

## System Design Philosophy

This is a C++20 Vulkan-HPP 1.4 real-time renderer designed for experimentation
with modern rendering algorithms. The architecture prioritizes:

- **Strict layer isolation** - Renderer ↔ Editor ↔ UI ↔ Asset
  boundaries must not be violated
- **Minimal global state** - State is explicit and localized
- **Explicit data flow** - Communication via UIEvents (Qt Signals/Slots),
  EventQueue (typed event dispatcher), and Context objects
- **Stateless rendering** - Renderer does not own application-level state
- **Deterministic GPU resource management** - Vulkan resources have explicit
  RAII ownership via `vk::raii` namespace

## Three-Layer Architecture + Supporting Modules

```
┌──────────────────────────────────────────────────────────────┐
│                   UI Layer (Qt6)                              │
│  (Window, surface, UIEvents, user input)                    │
└──────────────────────┬───────────────────────────────────────┘
                       │ Qt Signals/Slots (UIEvents)
                        │ + Typed Events (EventQueue)
                       ▼
┌──────────────────────────────────────────────────────────────┐
│                Editor Layer                                  │
│  (Application logic, controllers, scene state, event system)│
└──────────────────────┬───────────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────────┐
│               Renderer Layer (Vulkan-HPP vk::raii)           │
│  (All GPU resources: device, swapchain, pipeline,            │
│   buffers, images, descriptors, compute & geometry passes)  │
├──────────────────────────────────────────────────────────────┤
│               Asset Layer (src/asset/)                       │
│  (OBJ mesh loading, PNG/HDR decoding, CPU-side data)        │
└──────────────────────────────────────────────────────────────┘

Scene objects (src/scene/) and Core utilities (src/core/, src/project/)
are shared across layers.
```

### Layer Responsibilities

**Renderer Layer** (`src/render/`)
- Pure rendering service with no application logic
- Owns ALL GPU resources (device, swapchain, pipeline, command buffers, buffers, images, descriptors, RenderCache, Barrier state mapping)
- Owns VkDevice, VkSwapchainKHR, VkPipeline, VkCommandPool
- Consumes read-only VkSurfaceKHR from UI layer
- Consumes per-frame RenderContext (immutable scene snapshot) for pass dispatch
- Centralized image barrier management via `Barrier::Transition` (ImageState → Vulkan layout/stage/access)
- Owns `MeshGPU` (GPU-side mesh resources: VertexBuffer + IndexBuffer) via `RenderCache::GetMeshGPU()`
- Owns `EnvironmentGPU` (GPU-side IBL resources: diffuse + specular cubemap Textures) via `RenderCache::CreateEnvironmentGPU()`
- Owns `LightingCache` (light SSBOs: point + sun) via `RenderCache::InitLightingCache()` / `GetLightingCache()`
- Must NOT mutate application state
- Must NOT depend on Editor or UI layers

**Editor Layer** (`src/editor/`)
- Contains application logic and scene mutation
- Owns Controllers (CameraController, SceneController, ShaderController) via `src/editor/controllers/`
- Manages EditorContext (scene + editor state)
- Owns UIEvents (Qt signals) and EventQueue (typed EventPool)
- Communicates with Renderer via Context and typed EventQueue
- Must NOT directly manipulate GPU resources
- Controller registry pattern: `Editor::RegisterController<T>(bus)` creates controller, calls `Init(bus)`, stores in `m_controllers`
- Event-driven controller communication: `Editor::Edit(input)` translates InputState → typed events → EventQueue.Process() → controllers handle events

**UI Layer** (`src/ui/`)
- Qt6 Widgets presentation layer with Qt-Advanced-Docking-System (ADS)
- **UIManager**: QMainWindow subclass with dock manager, menus, and per-frame Refresh pipeline
- **Panel system**: all dock widgets inherit `UIPanel` (`PanelType` enum for registry): Viewport, Outliner, PropertyEditor, RenderConfigPanel
- `src/ui/items/`: Reusable composite QWidgets (ScalarSlider, Vec3Spin, OutlinerRow, ShaderFieldRow, CodeEditor)
- `src/ui/models/`: Qt item models (ShaderStructModel, LogModel, LogFilterProxy)
- `src/ui/delegates/`: Qt item delegates (ShaderFieldDelegate, LogDelegate)
- `src/ui/utils/`: Non-widget UI helpers (ShaderHighlighter)
- Displays data and captures user input
- Emits Qt signals via UIEvents for state changes
- Must NOT directly access Renderer internals
- Must NOT mutate scene state directly

**Asset Layer** (`src/asset/`)
- Vulkan-free: no `<vulkan/>` includes in public headers
- OBJ mesh loading and parsing into MeshData (pure CPU struct)
- PNG/HDR image decoding into ImageData (pure CPU struct, owning pixel vector)
- `PixelFormat` enum for CPU-side format queries (maps to `vk::Format` by convention, not by cast)
- CPU-side data representation only; GPU upload handled by Renderer via `MeshGPU` and `Image::FromImageData`
- Must NOT issue draw calls, manage GPU pipelines, or own GPU resources

**Scene Layer** (`src/scene/`)
- Vulkan-free in public interface: no GPU types exposed to consumers
- Scene objects (Camera, Light, Transform, Mesh, UID) define logical scene structure
- GPU resources (VertexBuffer, IndexBuffer) are separated into `MeshGPU` owned by RenderCache
- `Mesh::UploadToGPU()` bridges scene data to Renderer-owned GPU resources

### Communication Protocols

See `.github/instructions/events.instructions.md` for the complete event system
(UIEvents, EventQueue, event structs, and the `ConnectUIEvent`/`OnUIEvent`
template forwarding pattern).

Event structs in `src/editor/events/` are split by domain:
`SceneEvents.h` (scene-domain events carrying plain `int objectUid`:
selection, transform, visibility, camera/mesh/light/env property edits),
`EditorEvents.h` (cross-component events: RenderResetEvent,
EnvironmentChanged, SceneModified, LightGpuChanged, LightingRebuild,
SceneObjectGpuUploadRequested), and
`AssetEvents.h` (asset add/import: mesh/camera/light adds). Scene mutations are
handled by `SceneController`, which emits `EditorEvents` for GPU uploads and
dirty tracking; the Editor executes those uploads.

Cross-layer traffic flows along three event paths (see events.instructions.md,
"Three event paths"). On the UI->Editor path the UI emits PURE INPUT INTENTS
(`InputEvents.h`: mouse events, `ObjectClicked { int objectUid }`,
`DeleteRequested`) that carry no scene; the Editor wraps them into dedicated
scene events (`ObjectSelected`, `ObjectDeleteRequested`), and controllers
handle complete scene mutations carrying `int objectUid`, resolving the UID to
the live object via their `ControllerContext` (never raw pointers — a UID
stays stable after deletion while a pointer would dangle). The scene itself is
not carried in events: scene-scoped handlers obtain it from the context.

**UIEvents System** (Qt Signals)
- QObject singleton with typed Qt signals
- UI panels emit their own signals (e.g. `Outliner::objectClicked`, `RenderConfigPanel::configValueChanged`)
- `Application::ConnectUIEvent<T>` template bridges panel signals → `Editor::OnUIEvent<T>` → `EventQueue::enqueue<T>`

**EventQueue System** (Typed Event Dispatcher)
- Header-only template-based event dispatcher (no Qt dependency)
- Editor↔Renderer event dispatch with deferred `Process()`

**Context System** (Data)
- `EditorContext` - Scene + editor state
- `UIContext` - Per-frame UI snapshot carrying `RenderConfig` pointer
- Read-only access to scene data for Renderer
- Data flows: Editor mutates, Renderer consumes
- `RenderConfig` owned by Editor directly (no longer via Project);
  per-frame snapshots via `RenderContext::config` (`void*`)
- `Editor::SetRenderConfig()` writes UI config changes directly to Editor-owned RenderConfig → RenderContext

### Vulkan Ownership Graph (Critical)

```
UI Layer owns:
  QWindow → VkSurfaceKHR

Renderer Layer owns:
  VkInstance (shared via QVulkanInstance)
  VkDevice + VkQueue
  VkSwapchainKHR (consumes UI's VkSurfaceKHR)
  VkPipeline + VkPipelineLayout
  VkCommandPool + VkCommandBuffers
   RenderCache (cross-frame mutable resource pool, lazy GetAttachment / GetShadowMap / GetShadowIntensityArray)
   All framebuffer attachments (via RenderCache, dynamic rendering)

   Also through src/render/ abstractions:
   VkBuffer + VkDeviceMemory pairs (Buffer hierarchy: GPUBuffer, StagingBuffer, UniformBuffer<T>)
   VkImage + VkDeviceMemory + VkImageView triples (Image, Texture)
   VkDescriptorPool + VkDescriptorSet (DescriptorManager)
   Barrier::Transition (centralized layout transitions, ImageState → Vulkan mapping)
   MeshGPU (GPU-side mesh resources: VertexBuffer + IndexBuffer, owned by RenderCache)
   EnvironmentGPU (GPU-side IBL resources: diffuse + specular cubemap Textures, owned by RenderCache)
   LightingCache (GPU-side light SSBOs: point + sun light, owned by RenderCache)
```

Scene and Asset layers own NO GPU resources. `Mesh::UploadToGPU()` delegates to
`RenderCache::GetMeshGPU()` which creates and owns the `MeshGPU`. `ImageData`
provides CPU-side pixel buffers; GPU upload goes through `Image::FromImageData`.
The `GeometryRenderItem` struct that previously mixed CPU and GPU concerns has
been removed.

## Design Constraints

### Architectural Invariants

1. **No cross-layer direct coupling** - Use UIEvents/EventQueue/Context only
2. **Renderer is stateless** - Application state lives in Editor
3. **Full RAII** - No two-phase initialization; no `Init()`/`Terminate()` methods
4. **Explicit GPU ownership** - Each Vulkan handle has one owning layer
5. **Non-copyable GPU resources** - `= delete` copy/assign
6. **Vulkan validation** - Debug builds enable `VK_LAYER_KHRONOS_validation`

### Naming Conventions

- Classes: PascalCase (e.g., `VulkanContext`, `Shader`)
- Functions: PascalCase for public API; camelCase for internals
- Members: `m_` prefix for member variables
- Constants: UPPER_SNAKE_CASE
- Files: PascalCase for headers/sources

### Formatting / Includes

- Indentation: tabs
- Braces: Allman style
- Includes: local headers before system; group local, third-party (Vulkan, Qt,
  GLM), STL; use `#pragma once`

### Ownership / Error Handling

- All resource-owning classes follow RAII
- Classes owning GPU or OS resources delete copy constructor and assignment
- Use `std::unique_ptr`/`std::shared_ptr` for explicit ownership
- Raw pointers for non-owning references only
- Debug builds: Vulkan validation layers enabled
- Handle `VK_ERROR_DEVICE_LOST` and `VK_ERROR_OUT_OF_DATE_KHR` gracefully

### Coordinate System Convention

- **Z-up, +Y forward, right-hand** coordinate system (Blender convention)
- Up: `+Z`, Forward: `+Y`, Right: `+X`
- Euler rotation storage: `m_rotation = (pitch=X, roll=Y, yaw=Z)`
- Rotation order: `T * Rz(yaw) * Rx(pitch) * Ry(roll) * S`
- `Transform::GetDirection()` rotates `(0,1,0)` forward vector
- View space is left-handed (`GLM_FORCE_DEPTH_ZERO_TO_ONE`): `+Z` forward, NDC `[0,1]`
- Camera projection flips Y for Vulkan NDC: `proj[1][1] *= -1`
- See AGENTS.md for full reference.

### Current Scope

The project delivers a deferred PBR renderer with geometry pass, lighting
compute pass, and full G-Buffer pipeline through the four-layer architecture.

**Features in scope:**
- Vulkan-HPP RAII instance, device, swapchain, pipeline
- VK_KHR_dynamic_rendering for render passes
- Qt6 Widgets window with Qt-Advanced-Docking-System (ADS)
- Viewport as dockable central widget
- Qt Signals/Slots UIEvents singleton (UI↔Editor)
- Typed EventQueue for Editor↔Renderer event dispatch
- Swapchain recreation on window resize
- Validation layers in Debug builds
- Embedded SPIR-V shaders (compiled at CMake time)
- OBJ mesh loading with MeshData
- `PixelFormat` enum for CPU-side format queries (Vulkan-free asset layer)
- `MeshGPU` and `EnvironmentGPU` as RenderCache-owned GPU resources (separated from scene/asset layers)
- GPU-side mesh resources separated from scene `Mesh` (`MeshGPU` owned by RenderCache)
- `GeometryRenderItem` removed (CPU/GPU concerns now fully separated)
- Deferred PBR pipeline: ShadowDepthPass → GeometryPass (G-Buffer) → SSAOPass → Light+ShadowIntensity → LightingPass → IBLPass → SelectionOutlinePass (edge highlight) → ComposePass (gamma correction) → FXAAPass (FXAA 3.11, conditional) → DebugPass (overlay, LOAD_OP_LOAD onto the last post-chain image) → Blit to swapchain
- Centralized image barrier system (Barrier::Transition, ImageState enum)
- Screenshot capture + TextureData PNG readback
- GPU tests with shared VulkanTestShared base class
- Reference-image regression tests (capture → compare PNG)
- Render caches: RenderCache, DescriptorCache
- Pass base class with PassType enum + static query helpers; GeometryPass owns BeginPass/EndPass

**Features out of scope:**
- glTF/PNG file loading, asset pipeline (OBJ loading is in scope)
- SSR (Screen-Space Reflections)
- Ray tracing, mesh shaders
- Undo/redo, serialization, plugin system
- Linux/macOS support
- Threading, VMA, profilers, shader hot-reload

## Future Architecture Evolution

- Multi-pass deferred rendering pipeline
- PBR material system
- GPU-driven rendering with indirect draws
- Threaded command buffer recording
- Vulkan Memory Allocator (VMA) integration
- Asset pipeline (glTF loading, texture compression)
- Render graph for automatic barrier/resource management
- Ray tracing extensions
