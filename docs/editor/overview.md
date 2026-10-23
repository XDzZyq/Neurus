# Editor Layer

The Editor layer holds Neurus's application logic and scene mutation: it owns the
scene, the controllers, and the render configuration, and it orchestrates every
state change through the event system.

## Responsibilities

Editor work falls into four groups.

### Event management

The Editor subscribes to UI intents and typed events and dispatches them to
controllers. The full event system is described in [Events](events.md): the
`UIEvents` singleton, the `EventQueue` typed dispatcher, the event structs, and
the `ConnectUIEvent`/`OnUIEvent` template forwarding pattern.

`Input::Key` grows one key at a time, alongside the handler that needs it - a new
enum value is added only together with its consumer.

### Context provisioning

The frame's context is two views over the same document state - `RenderContext`
(`src/render/RenderContext.h`) for the Renderer and `UIContext`
(`src/ui/UIContext.h`) for the UI - and the Editor is the single producer of
both. It fills them and the other layers read. See
[RenderContext and UIContext](#rendercontext-and-uicontext).

### Controller orchestration

The `Controllers` base class declares `virtual Init(ControllerContext& ctx)`,
which binds a controller to its context. `Editor::RegisterController<T>()` is the
template factory that creates a controller, calls `Init(m_ctx)`, and stores it in
`ed_controllers`. Controllers receive discrete events rather than polling per
frame; `Editor::Edit()` dispatches all enqueued events through
`EventQueue::Process()`. See [Controllers](controllers.md).

### GPU upload and asset import

The Editor handles asset import events (mesh, camera, and light adds) and drives
GPU work - mesh upload, light SSBO updates, IBL (re)generation, per-mesh pipeline
warming, scene-resource eviction - without ever touching the GPU itself. It
subscribes to the cross-component `EditorEvents` (`LightGpuChanged`,
`LightingRebuild`, `SceneModified`) emitted by `SceneController`, converts the
work into typed `RenderRequest` values, and posts them through
`DeferredRenderer::Post()`. The renderer drains the queue per frame and executes
each request on the thread that submits (see
[Data Flow](../architecture/data-flow.md#render-request-path-editor-to-renderer-gpu-work)).
The Editor holds no `UploadManager` and never calls Vulkan; scene mutations go
through `SceneController`.

## Layer boundaries

Editor MAY:

- Mutate scene objects
- Own controllers and managers
- Emit and subscribe to `UIEvents` signals and `EventQueue` typed events
- Update the frame views (`RenderContext` / `UIContext`)
- Call into scene management systems

Editor MUST NOT:

- Directly manipulate GPU resources
- Call Vulkan functions
- Depend on UI implementation details (only `UIEvents` signals)
- Store rendering-specific state (it belongs in the Renderer or the
  Data/Resource layer)

## Communication with other layers

Across the whole program, data and control follow one unidirectional isolation
chain:

```
UI Layer (Qt6) -> UIEvents (Qt signals) -> Editor -> EventQueue (typed events) -> Controllers -> Context -> Renderer
```

Each arrow is a decoupled interface (signals, events, contexts); no layer knows
the internals of the next. The UI emits signals and never calls the Editor or the
Renderer directly. The Editor translates raw input into typed events and
dispatches them to controllers. The Renderer receives read-only scene data
through Context objects and never calls back into the Editor. See
[Data Flow](../architecture/data-flow.md) for the chain and the context objects
in full.

With the Renderer:

- Provides scene data via `RenderContext` (and `UIContext` to the UI)
- Receives rendering events via `UIEvents` (Qt signals)
- Posts GPU work as typed `RenderRequest` values through a `RenderRequestSink*`
  (Post only); its `EventQueue` stays internal to the Editor
- The Renderer never calls back into the Editor directly; the dependency is
  one-way, Editor to Renderer, through `RenderRequest`s and the frame snapshot

With the UI:

- The UI emits signals via `UIEvents`
- The Editor subscribes to UI-relevant signals
- The UI reads the published `UIContext` (scene and config) for display
- The link is two-way through `UIEvents`, never direct C++ calls

With the Data and Resource layer:

- The Editor may request resource creation (for example, "load this mesh")
- The Data layer performs the allocation and returns a handle
- The Editor stores the handle and passes it to the Renderer

## What the Editor owns

The Editor owns the `Scene`, the `RenderConfig`, the `Context`, the
`ResourceManager` (the application-scoped UID object pool), and the controllers.
It exposes `SetRenderConfig()` for UI-to-Editor sync, and an explicit scene-load
lifecycle (`NewScene()`, `CreateDefaultScene()`, `BeginLoad()`, `FinishLoad()`)
for Application-driven persistence. The Editor no longer owns the project file
path or performs Save/LoadProject; `Application` coordinates persistence and owns
the project path plus dirty aggregation. `Editor::IsDirty()`/`ClearDirty()` still
track scene and config edits.

The viewport is owned by the Editor but is its own composition root. See
[Viewport](viewport.md).

## RenderContext and UIContext

The frame's context is two views over the same document state, each produced by
the Editor. The renderer's view is an owned copy of the frame; the UI's view is a
pair of borrowed pointers.

```cpp
// src/editor/Editor.h
RenderFrameSnapshot BuildSnapshot() const;  // the whole renderer-facing frame, by value
void UpdateUIContext(UIContext& ctx) const; // scene, config
```

`UIContext` is forward-declared, so that half of the header pulls in nothing; but
`BuildSnapshot()` returns a `RenderFrameSnapshot` by value, so `Editor.h` does
include `render/RenderFrameSnapshot.h` for the complete type.

`RenderContext` is the renderer's view, and it has two writers. The Editor
publishes one owned frame snapshot; `DeferredRenderer::recordFrame()` stamps the
frame half. Every field carries a banner naming the half that fills it.

```cpp
// src/render/RenderContext.h - Vulkan-free
struct RenderContext
{
    // Editor half - the owned frame published by Editor::BuildSnapshot()
    const RenderFrameSnapshot* snapshot = nullptr;  // read through Frame(), never directly

    // Renderer half - stamped by DeferredRenderer::recordFrame()
    uint32_t  width      = 0;   // acquired image extent, not the surface size
    uint32_t  height     = 0;
    uint32_t  frameIndex = 0;   // ring-buffer slot
    glm::vec3 jitter{0.0f, 0.0f, 0.0f};  // per-frame shadow jitter
    uint32_t  iteration  = 0;   // temporal accumulation counter

    bool HasSnapshot() const;
    const RenderFrameSnapshot& Frame() const;  // never null; empty frame when unpublished
};
```

`RenderFrameSnapshot` (`src/render/RenderFrameSnapshot.h`) is an owned deep copy
of the frame - values only, no Scene pointers and no lazy reads. It carries
`config`, a `CameraSnapshot camera` (`view`/`projection`/`position`/`target`),
`meshes` and `lights` ordered by UID, `environmentId`, `activeObjectId`, and the
`debugDraw`/`gizmoDraw` lists, all by value, plus the predicates `HasScene()`,
`HasCamera()`, `HasEnvironment()`, `HasDebugDraw()` and `Settings()`. A pass reads
`ctx.Frame().Settings()`, `ctx.Frame().HasScene()`, `ctx.Frame().camera.view`,
`ctx.Frame().activeObjectId`, and so on. `Frame()` is never null: an unpublished
context reads as an empty frame - no scene, no camera, no draws, and a
default-constructed `RenderConfig` - which is the same null policy the old config
pointer's `Settings()` used to provide, so no pass tests a pointer.

`UIContext` is the UI's view, two Editor-owned fields wide, plus the
Application's frame-scoped half:

```cpp
// src/ui/UIContext.h - Qt-free
struct UIContext
{
    // Editor half - assigned by Editor::UpdateUIContext()
    const Scene*         scene   = nullptr;  // check HasScene()
    const RenderConfig*  config  = nullptr;  // read via Settings(), never directly

    // Application half - frame-scoped and service-backed
    const FrameProfile*  profile = nullptr;
    const LogBuffer*     log     = nullptr;
    const HistoryView*   history = nullptr;

    bool HasScene() const;
    const RenderConfig& Settings() const;      // defined in UIContext.cpp
};
```

Design:

- **The Editor is the pivot.** It produces both views and neither consumer
  reaches around it for the other's data. `BuildSnapshot()` copies the frame out
  of the Scene: `config`, the viewport's published camera (see
  [Viewport](viewport.md)), every mesh and light in UID order, the environment,
  the active object, the debug list when `RequiresDebugDraw()`, and the gizmo
  list unconditionally. The Application builds a fresh snapshot each tick and
  moves it into `RenderThread::Publish` (a latest-wins `FrameMailbox`); the
  render thread points `RenderContext::snapshot` at the frame it acquired, and
  `DeferredRenderer::recordFrame()` then stamps the frame half (`width`,
  `height`, `frameIndex`, `jitter`, `iteration`) into a **local copy**, which is
  what keeps temporal state renderer-owned - the Editor can neither read nor
  reset it. Width/height look like they could go either way and cannot: the
  extent a pass must use is the acquired image's, not the surface size the
  viewport knows, and the two disagree during a resize.
- **The frame is owned, not borrowed.** The renderer must be able to read the
  frame while the Editor edits on, and a borrowed pointer into the Scene cannot
  survive that - the Editor may re-key or free the pool entry it points at - but
  an owned copy can. `BuildSnapshot()` resolves every scene object to the stable
  UID the renderer's caches are keyed by and copies every matrix and parameter by
  value, so nothing in the frame dereferences the Scene after it is built (see
  `render/RenderFrameSnapshot.h`).
- `UpdateUIContext()` assigns fields only and must never call a `UIContext`
  method: it runs in `neurus_editor`, which does not link `neurus_ui` (that
  library links this one), so a symbol defined there would not resolve. The
  Application adds the frame-scoped `profile`/`log`/`history` members.
- Keeping the UI's view a separate two-field struct is what makes "the gizmo is
  not scene data" a property of the type rather than a convention.
- **The fallback is one value, reached two ways.** `UIContext::Settings()` is
  never null: with no published config it returns `DefaultRenderConfig()`
  (`src/core/RenderConfig.h`), which *is* `RenderConfig`'s in-class member
  initializers rather than a second copy of them. On the render side the frame's
  `config` is a value, default-constructed when nothing was published - and a
  default-constructed `RenderConfig` *is* that same `DefaultRenderConfig()` value
  - so `ctx.Frame().Settings()` needs no fallback at all. Either way a pass and a
  panel resolve an unset value identically, and no reader needs a fallback
  literal of its own.
- `UIContext`'s members are typed pointers named by forward declaration, so that
  header pulls in nothing yet no consumer casts; `RenderContext` carries the
  frame as one forward-declared `RenderFrameSnapshot` pointer. Growth is by a new
  typed field on the snapshot or on whichever view owns its source, never by a
  `void*` slot.
- `UIContext`'s pointers are borrowed and frame-scoped: valid only for the frame
  its producer filled them in, never stored across frames. `RenderContext`'s
  snapshot is the opposite by design - an owned copy a reader may keep while the
  Editor edits on - though `RenderContext` itself points at Application-owned
  storage for the current frame.
- Null policy: on `RenderContext`, `Frame()` is never null and `Settings()` is
  always safe; the frame's `HasScene()`, `HasCamera()` and `HasDebugDraw()` gate
  the lists before a reader walks them. On `UIContext`, `config` is always safe
  through `Settings()` and `scene` MUST be checked with `HasScene()`.

## Scene lifecycle

### New scene vs. new project: two steps, composed by the Application

`NewScene()` means empty. No mesh, no light, no scene camera, nothing in the
Outliner. It drops the outgoing scene's GPU resources, replaces the `Scene`,
clears the pool, the undo history and the `RenderConfig`, re-mints the viewport
camera, and leaves `m_dirty` false. It takes no `objPath` and loads no asset.

`CreateDefaultScene(objPath)` is purely additive. It adds a deactivated scene
camera, the starter mesh, a point light, an IBL environment, and one debug object
of each kind to whatever scene is current. It resets nothing and must not be
expected to: calling it twice layers a second copy on top
(`test_editor_scene_lifecycle.cpp` pins exactly that).

File > New is the pair, not either half. `Application::OnProjectNew()` runs
`NewScene()`, then `CreateDefaultScene(kDefaultSceneObj)`, then `ClearDirty()`,
then one `UploadSceneResources()`. The `Run()` first-launch fallback composes the
same two steps and lets `OnProjectSave()` rebase the dirty flag. Why New seeds
content rather than nothing: an empty scene renders solid black with an empty
Outliner, which is indistinguishable from a crash. The minimum legal scene and
the minimum useful scene are not the same thing, and the menu owes the user the
latter, but that is the Application's judgement about a new project, not a
property of a new scene, which is why the Editor keeps the two apart.

The dirty flag follows the same seam: `CreateDefaultScene()` marks the scene
dirty like any other mutation, and whoever means that content as the document's
saved baseline rebases with `ClearDirty()`. The Editor does not guess which one
it is.

The starter scene still ships a scene camera as demo content, deliberately left
deactivated. Both start at the same pose, so this is invisible until the user
navigates. `Application` passes the same `kDefaultSceneObj` constant to both
callers of `CreateDefaultScene()` so they cannot drift.

Every scene object also sets `o_name` in its constructor (`"Camera"`, `"Mesh"`,
`ParseLightName()` for typed lights, `"DebugLine"`, and so on). A blank `o_name`
renders as an empty Outliner row that looks broken;
`test_editor_scene_lifecycle.cpp` asserts no seeded object leaves it empty.

`kStarterMeshScale` fixes the starter scene's scene-space numbers: the camera
sits at 5.4 units, the axes span +-4, the light box is 0.4 wide, and the scale
itself is 1/7.44 ~= 0.135. GPU tests scale the same asset by 0.25.

### Uploading the scene

Neither lifecycle step uploads. `UploadSceneResources()` is the single "make this
scene drawable" step: meshes, lights, debug meshes, the environment's IBL
cubemaps, and the light SSBO. Every part of it skips work that is already cached,
so calling it twice is nearly free. Keeping it in one piece is the point: it used
to be two calls, and the first-launch fallback (whose scene never passes through
`FinishLoad()`) ran only the first of them. The scene then held an Environment
whose properties read correctly in the Property panel while its `EnvironmentGPU`
never reached the render cache, so the scene rendered unlit. The caller owns the
timing because the first-launch path builds its scene before `show()`, where an
upload loses the surface, so `Run()` uploads once after `show()`, while
`OnProjectNew()` uploads immediately.

Replacing a scene drops the outgoing scene's GPU resources first.
`DropSceneGpuResources()` posts a `DropSceneResourcesRequest` and is called by
`BeginLoad()` and `NewScene()`. The drain and the eviction happen renderer-side:
the executor calls `device.waitIdle()` (the entries own `vk::raii` resources the
last frame may still read) and then `RenderCache::RemoveSceneResources()`. What
the Editor still guarantees is ORDER - this post precedes the new scene's upload
posts, and FIFO execution evicts the old scene before the new one uploads. This
is not housekeeping: the caches are keyed by object UID and `UID::serialize()`
restores `o_id` from the file, so an entry left behind shadows whatever returns
under the same id. Load project A, then project B, and B draws A's geometry,
shadow maps and cubemaps while its own properties are displayed correctly beside
them. The same reasoning is why the scene-upload environment post is
skip-if-cached (`regenerate = false`) while a live environment change
(`EnvironmentChanged`, an environment re-entering the scene) posts with
`regenerate = true` to force the cubemaps to rebuild.

`DeferredRenderer::DrawFrame()` still carries a null-camera precondition check as
defense in depth: it skips the frame and logs once (`m_reportedNoCamera`) instead
of faulting. That guard is a diagnostic, not a license. `GetCamera()` cannot
return null unless a scene-mutation path broke the viewport contract (see
[Viewport](viewport.md)), and that path is the bug.
`test/editor/test_viewport_camera.cpp` pins the whole contract (GPU-free, runs in
CI): pooled-but-not-scene-content, seeded with a usable pose, rebuilt after every
pool clear, identity and pose surviving save/reopen (including from a file
written under the pre-rename `editorCamUid` key), a camera-less project loading
with nothing injected, deleting every scene camera, activation taking the view
with deactivation handing it back at the pose it was left at, and the resize path.

### Restoring the pool on load

`Editor::FinishLoad()` restores the resource pool before the scene is usable:
`ResourceComponent` wires the per-object data references as the pool loads, and
each `RenderShader` re-parses and recompiles inside its own `serialize`.

## Data flow

```
User Input (QML) -> UIEvents Signal -> Editor Subscriber -> State Update
                                                              |
                                                       EventQueue.enqueue
                                                              |
                                                       EventQueue.Process()
                                                              |
                                                       UI Refresh
```

The frame loop, main thread then render thread:

```
UIEvents::newFrame() -> Editor::Edit(input) -> Editor::BuildSnapshot()
                     -> RenderThread::Publish(snapshot)        (never blocks)
render thread:          fence -> controls -> requests -> Acquire -> DrawFrame
```

Example: window resize

1. The Viewport panel emits `resized(w, h)` (logical pixels)
2. `Application::ResizeViewport` resizes the Metal layer (AppKit, main thread),
   posts `RenderThread::PostResize(w, h)` and hands `EditorViewport` the
   logical size plus an estimated physical extent (logical x device pixel ratio)
3. The render thread rebuilds the swapchain between frames (only the last
   resize of a batch runs) and reports its extent through `Status()`; the next
   tick corrects the viewport's physical extent if the swapchain chose another

The event-by-event breakdown of how a panel signal becomes a controller call is
in [Events](events.md).

## Scope

- `UIEvents` singleton with UI-to-Editor signals (`newFrame`, `windowResized`,
  and so on)
- `EventQueue` typed event dispatcher for Editor-internal events (input to Controllers)
- `RenderRequest` posting through `RenderRequestSink` for Editor-to-Renderer GPU work
- `RenderContext`: the renderer's view - one owned `RenderFrameSnapshot` (built
  by `Editor::BuildSnapshot()`) plus the frame half stamped by the renderer
- `UIContext`: the UI's view (scene and config), produced by
  `Editor::UpdateUIContext()`
- Editor orchestrator with `RegisterController<T>()` and `Edit()` for event
  dispatch
- `Controllers` base class with the `Init(ControllerContext&)` virtual interface
  (`IEventQueue` + `IResourceLookup` + `IOperationSink` + scene/config providers)
- `CameraController`: event-driven orbit/zoom/dolly/pan via `CameraEvents` (int
  `camId` payloads)
