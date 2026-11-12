# Layers

The application, renderer, editor, script, UI, asset and scene layers each own a
distinct responsibility, and Core supplies utilities shared across all of them.

## Renderer Layer (`src/render/`)

- Pure rendering service with no application logic
- Runs on its own thread: `RenderThread` owns the frame mailbox and the control
  queue and is the only caller into the renderer once started. The main thread
  publishes a `RenderFrameSnapshot` and posts control messages (resize, surface
  change, pick, screenshot)
- Borrows the `VkSurfaceKHR`, `VkDevice`, physical device and graphics queue
  from the Application; it creates no instance or device
- Owns the presentation chain: `Swapchain` wraps `VkSwapchainKHR` and its image
  views, recreates them on a resize or an out-of-date acquire, and presents
  through the shared graphics queue
- Owns every GPU resource behind that image: `RenderCache` and its attachments,
  the passes and their pipelines, command pools and buffers, buffers, images,
  descriptors, Barrier state mapping
- Owns `OverlayEngine`, a separate `RenderGraph` over the scene graph's tail
  image. It runs SelectionOutlinePass -> OverlayCopyPass -> DebugPass ->
  GizmoPass into `OverlayOutput`, which the renderer then blits to the
  swapchain. The scene image is never presented directly, so an overlay-only
  change redraws without re-running a scene pass
- Builds the scene graph from `RenderConfig`: ShadowDepthPass, GeometryPass
  (G-Buffer), SSAOPass, ShadowIntensityPass, LightingPass, ComposePass and, when
  enabled, FXAAPass
- Consumes the per-frame RenderContext (the renderer's view of the frame: the
  Editor's owned frame snapshot plus the frame half `recordFrame()` stamps) for
  pass dispatch
- Drains posted `RenderRequest` values per frame through
  `RenderRequestProcessor` and `RenderRequestExecutor`: the Editor requests GPU
  work, it never submits
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
- Owns `OperationManager` (`src/editor/operations/`): undo/redo over
  event-replay `Operation` values. Controllers record through `IOperationSink`,
  undo and redo re-emit the inverse event synchronously, and a script run or a
  command is buffered into one history entry
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

## Script Layer (`src/script/`)

- Embedded CPython runtime (`ScriptEngine`), owned by the Application and run on
  the main thread. Its PIMPL body keeps Python and pybind11 headers out of every
  other layer
- Owns the interpreter, the `neurus` module and its bindings, the scene and
  object proxies, and the Script Console ring buffer
- Reads one `ScriptContext` per run: borrowed, non-owning `Scene`,
  `RenderConfig` and `IResourceLookup` pointers built by
  `Editor::UpdateScriptContext()`. There is no process-wide context
- Mutates only by emitting typed editor events: `ScriptDispatch::Emit()` hands
  the event to the Editor's `IEventQueue` synchronously, so the controller
  applies the change before the Python statement returns
- Has no project save and no snapshot capture; both stay CLI and UI operations
- Must NOT include Renderer or UI headers
- Must NOT call Editor methods or hold a persistent Editor pointer
- Vulkan-free and Qt-free

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
- Exposes the viewport's native window handle; creates no Vulkan object,
  including `VkSurfaceKHR` (the Application creates the surface from that
  handle)
- Must NOT directly access Renderer internals
- Must NOT mutate scene state directly

## Application Layer (`src/app/`)

- Composition root and process lifetime: creates `QApplication`, `Preferences`,
  `UIManager`, the `VulkanContext` (instance, device, queues), the editor, the
  renderer and its thread, the upload manager, the script engine and the
  session server
- Owns `VkSurfaceKHR`, created from the viewport's native handle with
  `platform::CreateSurface()`. On a viewport recreation it creates a new
  surface, asks the render thread to rebuild the swapchain, and only then
  destroys the old one
- Owns the project path and the project lifecycle (new, open, save)
- Runs the per-tick frame loop: drain session commands, run queued scripts,
  apply a finished pick, `Editor::Edit()`, build and publish the frame snapshot,
  then refresh the UI from `UIContext`
- Wires every channel: UI signals into `Editor::OnUIEvent`, viewport recreation
  into a new surface and swapchain, and the command side through
  `EditorCommandAdapter`
- Implements the command-side views (`ISessionState`, `IScriptCommands`,
  `ISnapshotCommands`)
- Must NOT implement rendering or scene mutation itself; it delegates to the
  Renderer and the Editor

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
- The application hosts the server in `src/app/session/`; it is the only
  Command-side code that uses Qt
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
