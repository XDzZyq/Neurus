# Layers

The renderer, editor, UI, asset and scene layers each own a distinct
responsibility, and Core supplies utilities shared across all of them.

## Renderer Layer (`src/render/`)

- Pure rendering service with no application logic
- Owns ALL GPU resources (device, swapchain, pipeline, command buffers,
  buffers, images, descriptors, RenderCache, Barrier state mapping)
- Owns VkDevice, VkSwapchainKHR, VkPipeline, VkCommandPool
- Consumes read-only VkSurfaceKHR from UI layer
- Consumes the per-frame RenderContext (the renderer's view of the frame: the
  Editor's owned frame snapshot plus the frame half `recordFrame()` stamps) for
  pass dispatch
- Centralized image barrier management via `Barrier::Transition` (ImageState ->
  Vulkan layout/stage/access)
- Owns `MeshGPU` (GPU-side mesh resources: VertexBuffer + IndexBuffer) via
  `RenderCache::GetMeshGPU()`
- Owns `EnvironmentGPU` (GPU-side IBL resources: diffuse + specular cubemap
  Textures) via `RenderCache::CreateEnvironmentGPU()`
- Owns `LightingCache` (light SSBOs: point + sun) via
  `RenderCache::InitLightingCache()` / `GetLightingCache()`
- Must NOT mutate application state
- Must NOT depend on Editor or UI layers

## Editor Layer (`src/editor/`)

- Contains application logic and scene mutation
- Owns Controllers (CameraController, TransformGizmoController,
  ShaderController, SceneController, RenderConfigController) via
  `src/editor/controllers/`
- Produces the two frame views: the renderer's view as an owned
  `RenderFrameSnapshot` via `BuildSnapshot()`, and `UIContext` (the UI's view)
  via `UpdateUIContext()`
- Owns `EditorViewport`, the viewport composition root: the **viewport camera**
  (a pooled `Camera` that is not scene content), the debug/gizmo builders and
  the resize. `EditorViewport::GetCamera()` publishes the activated scene
  camera when the Scene names one and the viewport camera otherwise, and the
  frame snapshot's camera carries that choice to the renderer, so no pass reads
  the Scene. The Editor itself holds no camera and exposes no camera API, only
  `GetViewport()` and the timing (`SyncCamera` in `Edit()`). A scene with zero
  cameras is a legal document. See [Editor Overview](../editor/overview.md).
- Owns UIEvents (Qt signals) and EventQueue (typed EventPool)
- Communicates with Renderer via Context and typed EventQueue
- Must NOT directly manipulate GPU resources
- Controller registry pattern: `Editor::RegisterController<T>(bus)` creates
  controller, calls `Init(bus)`, stores in `m_controllers`
- Event-driven controller communication: `Editor::Edit(input)` translates
  InputState -> typed events -> EventQueue.Process() -> controllers handle
  events

## UI Layer (`src/ui/`)

- Qt6 Widgets presentation layer with Qt-Advanced-Docking-System (ADS)
- **UIManager**: QMainWindow subclass with dock manager, menus, and per-frame
  Refresh pipeline
- **Panel system**: all dock widgets inherit `UIPanel` (`PanelType` enum for
  registry): Viewport, Outliner, PropertyEditor, RenderConfigPanel
- `src/ui/items/`: Reusable composite QWidgets (ScalarSlider, Vec3Spin,
  OutlinerRow, ShaderFieldRow, CodeEditor)
- `src/ui/models/`: Qt item models (ShaderStructModel, LogModel,
  LogFilterProxy)
- `src/ui/delegates/`: Qt item delegates (ShaderFieldDelegate, LogDelegate)
- `src/ui/utils/`: Non-widget UI helpers (ShaderHighlighter)
- Displays data and captures user input
- Emits Qt signals via UIEvents for state changes
- Must NOT directly access Renderer internals
- Must NOT mutate scene state directly

## Asset Layer (`src/asset/`)

- Vulkan-free: no `<vulkan/>` includes in public headers
- OBJ mesh loading and parsing into MeshData (pure CPU struct)
- PNG/HDR image decoding into ImageData (pure CPU struct, owning pixel vector)
- `PixelFormat` enum for CPU-side format queries (maps to `vk::Format` by
  convention, not by cast)
- CPU-side data representation only; GPU upload handled by Renderer via
  `MeshGPU` and `Image::FromImageData`
- Must NOT issue draw calls, manage GPU pipelines, or own GPU resources

## Scene Layer (`src/scene/`)

- Vulkan-free in public interface: no GPU types exposed to consumers
- Scene objects (Camera, Light, Transform, Mesh, UID) define logical scene
  structure
- GPU resources (VertexBuffer, IndexBuffer) are separated into `MeshGPU` owned
  by RenderCache
- `Mesh::UploadToGPU()` bridges scene data to Renderer-owned GPU resources

## Core Layer (`src/core/`)

Vulkan-free utilities shared across the other layers:

- `UID` - generic unique identifier primitive (resource base)
- `ResourceManager` - single factory + UID pool (`Load<T>`, polymorphic
  serialize)
- `IResourceLookup` - read-only pool lookup interface
- `Log` - `NEURUS_LOG` / `NEURUS_ERR` macros
- `RenderConfig` - user-settable pipeline options (Vulkan-free; read by four
  layers)
- `Serialize` - `archive::Optional` / `LoadFresh` / `OptionalBlock`
  (version-tolerant fields)
- `Graph` - generic DAG template
- `Selections` - selection state container
- `Timer` - scoped timer
