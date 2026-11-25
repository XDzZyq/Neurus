# Layers

The application, renderer, editor, script, UI, asset and scene layers each own a
distinct responsibility, and Core supplies utilities shared across all of them.

## Shared Render Contracts (`src/render_api/`)

`neurus_render_api` contains owned frame snapshots, render requests and their
write-only sink, context and profiling values, and CPU overlay payloads. It is
Qt-free and Vulkan-free and depends only on CPU asset data, scene and core types.
Editor and UI include these contracts without linking renderer implementation.
Graph requests carry an immutable `RenderGraphData` with shared CPU shader
references. Shader upload requests carry bytecode and metadata. The graph mapper
uses stable shader UIDs to resolve the uploaded GPU resources.

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
- Builds the runtime scene graph from the published `RenderGraphData`.
  `GraphUploader` maps pass kinds and cached GPU shaders to `PassFactory` instances,
  maps every node and connection through a `GraphMorphism` local to each compile, and its
  output tags plan the `RenderCache` resources. `RenderConfig` supplies
  per-frame pass parameters only
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
- Produces the renderer's owned `RenderFrameSnapshot` via `BuildSnapshot()`.
  Application queries the scene and config to assemble the UI-owned `UIContext`.
- Owns `RenderManager`. Its active graph pointer selects a pooled `RenderGraphData`.
  Null graph and config pointers select built-in defaults and omit their respective
  project keys on save. Reads preserve this state
- Prepares owned shader upload requests on the CPU, then publishes optional
  cloned graph topology through `CompileRenderGraphRequest{graph}` in FIFO order.
  Live nodes own pooled shader references; graph mapping reads only their stable
  UIDs and resolves cached GPU shaders. Built-in CPU preparation stays outside
  the project pool; Renderer owns the compiled graph and GPU resources
- Explicit config edits materialize stored settings. Undo restores the previous
  presence state, including null. Frame snapshots copy effective config by value,
  so the render thread never shares mutable Editor config storage
- Owns `EditorViewport`, the viewport composition root: the **viewport camera**
  (a pooled `Camera` that is not scene content), the debug/gizmo builders and
  the resize. `Editor::ActiveCamera()` selects the activated scene camera when the
  Scene names one and the viewport camera otherwise; `Editor::PublishActiveCamera()`
  hands that choice to `EditorViewport`, and the frame snapshot's camera carries it
  to the renderer, so no pass reads the Scene. The Editor exposes no public camera
  API, only `GetViewport()` and the timing (`PublishActiveCamera` in `Edit()`). A
  scene with zero cameras is a legal document. See
  [Editor Overview](../editor/overview.md).
- Owns the typed `EventQueue`; the Qt `UIEvents` bus lives in `ui/events/`
- The `EventQueue` is **Editor-internal**: it is not a channel to the Renderer
  and nothing under `src/render/` includes it. It is drained once per frame
  (`Edit()`) for events that originated at the UI boundary; everything else in
  the editor dispatches synchronously through `Emit`
- Communicates with the Renderer via Context (read-only frame snapshot) and typed
  `RenderRequest`s posted through a `RenderRequestSink*`
- Must NOT directly manipulate GPU resources
- Owns the project adapters for undo history and viewport state under
  `editor/components/`; asset persistence does not include Editor internals
- Controller registry pattern: `Editor::RegisterController<T>()` creates the
  controller, calls `Init(ed_ctx)` and stores it in `ed_controllers`
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
- Config bindings use the context's read-only provider on each access, so a
  config created or removed by a synchronous edit is visible within the same run
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
- Owns `ui/events/UIEvents` and the Qt input translation in `ui/Input.cpp`
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

- `ResourceManager` owns the UID pool and constructs resources. `ResourceType` maps cereal archive names to registered resource types.
- Vulkan-free and independent of Renderer and Editor implementation targets
- OBJ mesh loading and parsing into MeshData (pure CPU struct)
- PNG/HDR image decoding into ImageData (pure CPU struct, owning pixel vector)
- `PixelFormat` enum for CPU-side format queries (maps to `vk::Format` by
  convention, not by cast)
- CPU-side shader data: `ShaderData` under `data/`, with its IR/unit headers
  (`ShaderStruct`, `ShaderUnit`, `ShaderStage`, `ShaderStructSerialize`) under
  `data/shader/`. CPU shader values remain editable; concrete shader behavior
  belongs to the Shader module
- `ShaderData` owns shader source text IO and path resolution; Asset neither
  parses shader source nor compiles it
- CPU-side data representation only; GPU upload handled by Renderer via
  `MeshGPU` and `Image::FromImageData`
- Must NOT issue draw calls, manage GPU pipelines, or own GPU resources

The asset build separates `neurus_asset_data` (CPU resource values and source IO)
from `neurus_asset` (project persistence). Scene depends only on the data target;
persistence depends on scene and data, so the CPU targets have no asset/scene cycle.
Neither Asset target depends on shader tooling.

## Shader Module (`src/shader/`)

- CPU-only `neurus_shader`, depending on `neurus_asset_data` and shaderc
- Owns `RenderShader`, `ComputeShader`, shader parsing/generation/compilation,
  `ShaderLibrary`, and the built-in CPU data provider
- Consumes source text provided by Asset; file-loading convenience methods
  delegate text IO and path resolution to `ShaderData::ReadSource` and `ResolveSourcePath`
- Owns concrete shader cereal registrations; ApplicationCore force-links them
  before generic Asset persistence restores registered resource types
- Defines the `BuiltInRenderData` service. Each application shares one instance
  between the Editor and renderer. Its cached defaults stay outside the project.
- Preserves current shader serialization reload/recompile behavior
- Does not own Vulkan shader modules, pipelines, descriptor sets or renderer state

The shared `render_api` contracts carry owned shader upload payloads, graph topology and scene values;
they do not depend on the Shader module.

## Scene Layer (`src/scene/`)

- Vulkan-free target: no renderer or Vulkan linkage and no GPU types exposed to consumers
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
- `IResourceLookup` - pool fetch/lookup and name-registry interface
- `Log` - `NEURUS_LOG` / `NEURUS_ERR` macros
- `RenderConfig` - user-settable pipeline options (Vulkan-free; read by four
  layers)
- `Serialize` - `archive::Optional` / `LoadFresh` / `OptionalBlock`
  (version-tolerant fields)
- `Graph` - generic DAG template
- `Selections` - selection state container
- `Timer` - scoped timer
