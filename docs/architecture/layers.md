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
- Holds `MeshGPU` (GPU-side mesh resources: VertexBuffer + IndexBuffer) in a
  refcounted table (`RenderCache::UseMeshGPU()` / `Meshes()`)
- Holds `EnvironmentGPU` (GPU-side IBL resources: diffuse + specular cubemap
  Textures) in a refcounted table (`RenderCache::UseEnvironmentGPU()` /
  `Environments()`)
- Owns `LightingCache` (light SSBOs: point + sun) via
  `RenderCache::SetLightingCache()` / `GetLightingCache()`
- Must NOT mutate application state
- Must NOT depend on Editor or UI layers

## Editor Layer (`src/editor/`)

- Contains application logic and scene mutation
- Owns Controllers (CameraController, TransformGizmoController,
  ShaderController, SceneController, ObjectController, RenderConfigController,
  ScriptController) via `src/editor/controllers/`
- Produces the two frame views: the renderer's view as an owned
  `RenderFrameSnapshot` via `BuildSnapshot()`, and `UIContext` (the UI's view)
  via `UpdateUIContext()`
- Owns `EditorViewport`, the viewport composition root: the **viewport camera**
  (a pooled `Camera` that is not scene content), the debug/gizmo builders and
  the resize. `Editor::ActiveCamera()` selects the activated scene camera when the
  Scene names one and the viewport camera otherwise; `Editor::PublishActiveCamera()`
  hands that choice to `EditorViewport`, and the frame snapshot's camera carries it
  to the renderer, so no pass reads the Scene. The Editor exposes no public camera
  API, only `GetViewport()` and the timing (`PublishActiveCamera` in `Edit()`). A
  scene with zero cameras is a legal document. See
  [Editor Overview](../editor/overview.md).
- Owns UIEvents (Qt signals) and the typed `EventQueue`
- The `EventQueue` is **Editor-internal**: it is not a channel to the Renderer
  and nothing under `src/render/` includes it. It is drained once per frame
  (`Edit()`) for events that originated at the UI boundary; everything else in
  the editor dispatches synchronously through `Emit`
- Communicates with the Renderer via Context (read-only frame snapshot) and typed
  `RenderRequest`s posted through a `RenderRequestSink*`
- Must NOT directly manipulate GPU resources
- Controller registry pattern: `Editor::RegisterController<T>()` creates the
  controller, calls `Init(m_ctx)` and stores it in `ed_controllers`
- Event-driven controller communication: a UI signal reaches
  `Editor::OnUIEvent`, the next `Edit()` drains it to the controllers, and every
  event a handler emits is dispatched before that handler returns

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
- `src/ui/utils/`: Non-widget UI helpers (CodeHighlighter)
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
- GPU resources (VertexBuffer, IndexBuffer) are separated into `MeshGPU` held by
  RenderCache
- Scene objects never upload: the Editor posts an `UploadMeshRequest` carrying the
  `MeshData`, and the renderer owns the result

## Command Layer (`src/command/`, `src/ipc/`, `src/cli/`)

- `neurus_command` - Qt-free, Vulkan-free request/result values, stable error
  codes, command descriptors, the versioned JSON codec and the main-thread
  dispatcher
- `neurus_ipc` - Qt-free, Vulkan-free length framing, session registry, token
  generation, and the Asio loopback client/server
- `neurus_cli` (`neurus`) - Qt-free, Vulkan-free CLI11 parsing, process launch
  and output formatting
- The application hosts the server in `src/app/session/`; that is the only new
  code allowed to use Qt
- The Asio thread only accepts, reads and writes. Every command executes on the
  main thread in `Application::NewFrameSignals`, before scripts and
  `Editor::Edit()`, and mutates through the existing typed events and
  controllers
- The CLI never constructs an `Editor`; a request without a live session is an
  error

## Core Layer (`src/core/`)

Vulkan-free utilities shared across the other layers:

- `UID` - generic unique identifier primitive (resource base)
- `ResourceManager` - single factory + UID pool (`Load<T>`, polymorphic
  serialize)
- `IResourceLookup` - pool fetch/lookup and name-registry interface
- `Log` - `NEURUS_LOG` / `NEURUS_ERR` macros
- `RenderConfig` - user-settable pipeline options (Vulkan-free; read by four
  layers)
- `Serialize` - `archive::Optional` / `LoadFresh` / `OptionalBlock`
  (version-tolerant fields)
- `Graph` - generic DAG template
- `Selections` - selection state container
- `Timer` - scoped timer
