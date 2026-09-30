# Editor Layer

## Overview

The Editor layer contains **application logic and scene mutation**. It owns the
Controllers, manages user input, maintains selections, and orchestrates state
changes through the event system.

## Location

- `src/editor/Input.h` - Stateless static translation helpers only: `GetMousePos`, `GetModifiers`, `GetMouseButton`, `GetKey` (raw Qt values -> engine enums). There is no `InputState` struct and no per-frame polling — input reaches the Editor as typed events.
- `src/editor/Editor.h` - Editor orchestrator (owns Scene, RenderConfig, Context, Controllers)
  - Exposes an explicit scene-load lifecycle for Application-driven persistence:
    `NewScene(objPath)` (fresh document holding the **default starter scene** —
    see the invariant below), `BeginLoad()` (WaitIdle + fresh Scene/RenderConfig)
    and `FinishLoad()` (upload scene resources, IBL). Editor no longer owns the
    project file path or performs Save/LoadProject — `Application` coordinates
    persistence and owns the project path + dirty aggregation (`Editor::IsDirty()` /
    `ClearDirty()` still track scene/config edits).
- `src/editor/EditorContext.h` - Editor + scene state container
- `src/editor/controllers/Controllers.h` - Base class for all controllers
- `src/editor/controllers/CameraController.h` - Event-driven camera manipulation (orbit/zoom/dolly/pan)
- `src/editor/controllers/SceneController.h/cpp` - Event-driven scene mutations (selection, transform, visibility, property edits); emits EditorEvents for GPU uploads
- `src/editor/controllers/ShaderController.h` - Event-driven shader lifecycle (create, compile, code/struct edit, field add)
- `src/editor/controllers/TransformGizmoController.h/cpp` - Drives the modal G/R/S gesture; one undo entry per gesture
- `src/editor/events/ShaderEvents.h` - Shader editor event structs (see events.instructions.md)
- `src/editor/events/GizmoEvents.h` - Modal transform intents + the shared `GizmoMode`/`GizmoAxis` vocabulary
- `src/editor/viewport/DebugDrawBuilder.h/cpp` - Flattens the Scene's debug objects into the `DebugDrawList` the renderer consumes
- `src/editor/viewport/TransformGizmo.h/cpp` - Modal transform state machine + `GizmoAxisDirection` / `GizmoEulerFromRotation`
- `src/editor/viewport/GizmoDrawBuilder.h/cpp` - Gesture state + viewport -> `GizmoDrawList` (fixed pixel budget)
- `src/editor/viewport/EditorViewport.h/cpp` - World<->screen service: `Project`, `RayThrough`, `ProjectBox`, `PixelsPerWorldUnit`; holds the camera the viewport looks through

## The view camera: the Editor owns one, an activated scene camera overrides it

Every view-projection pass reads `ctx.editor.camera` — `ShadowDepthPass.cpp:439`,
`ShadowIntensityPass.cpp:598`, `SSAOPass.cpp:343`, `LightingPass.cpp:305`, plus
`GeometryPass` through the shared `CameraGPU` — and **no pass reads the Scene**.
The Editor decides what that pointer is, once per frame, in `Editor::Edit()`:

```
Editor::ViewCamera()  =  GetScene().GetActiveCamera()   // the activated scene camera
                         ?: m_editorCamera.get()        // otherwise the editor camera
```

So a scene containing **zero cameras is a legal document**, and deleting every
camera in one is a legal gesture. There is no last-camera guard and nothing is
injected on load to repair a camera-less project; both of those existed only to
prop up the old invariant, where the viewport looked through scene content.

**The editor camera is pooled but is not scene content.** It lives in the
`ResourceManager` so `CameraController::ResolveCamera` can resolve and orbit it by
UID like any other camera, and it is absent from `cam_list`, so it cannot be
selected, deleted, or activated — `Scene::ActivateCamera` validates membership in
`cam_list` and refuses it. Orbiting to inspect a model therefore no longer edits
scene content.

**It cannot be created once in the constructor.** `m_resources->Clear()` runs in
both scene-swap paths and drops every pooled object, so `EnsureEditorCamera()`
(resolve `m_editorCamUid` through the pool, else `Load<Camera>()` at `(0,-5,2)`
looking at the origin) runs on each of them:

- `CreateDefaultScene()` — after `Clear()`: reset the uid, then ensure. The uid
  therefore **changes on every File → New**, by design.
- `BeginLoad()` — after `Clear()`: drop the reference and the uid.
- `FinishLoad()` — ensure, then `ApplyViewportToViewCamera()`. A uid restored from
  the file resolves to the serialized camera; a missing one mints a fresh camera.

**Its identity is serialized, so the user's viewpoint survives a reopen.** The
camera *object* rides along in `ResourceComponent`'s wholesale pool serialization;
`project::EditorComponent` (key `"m_editor"`, registered **last** in
`Application::BuildProject` because registration order is archive read order)
records only which pooled camera the Editor claims as its own. Without that uid,
`FinishLoad()` would mint a fresh camera at the default framing and the view would
snap back on every open. A project written before the component existed simply
lacks the node; `Load()` falls back rather than throwing.

The starter scene still ships a scene camera as demo content, deliberately left
**deactivated** — both start at the same pose, so this is invisible until the user
navigates. `CreateDefaultScene(objPath)` is the single starter-scene builder
(fresh Scene, pool `Clear()`, editor camera, default RenderConfig, a deactivated
camera, mesh, point light, environment, demo debug objects); `NewScene(objPath)`
**delegates to it** rather than building an empty scene, then adds
`ed_operations.Clear()`, `m_dirty = false` and `UploadSceneResources()`.
`Application` passes the same `kDefaultSceneObj` constant to both paths so they
cannot drift.

Why New seeds content rather than nothing: an empty scene renders solid black with
an empty Outliner, which is indistinguishable from a crash. The minimum legal
scene and the minimum useful scene are not the same thing, and New owes the user
the latter.

Every scene object also sets `o_name` in its constructor (`"Camera"`, `"Mesh"`,
`ParseLightName()` for typed lights, `"DebugLine"`, …). A blank `o_name` renders
as an empty Outliner row that looks broken; `test_editor_scene_lifecycle.cpp`
asserts no seeded object leaves it empty.

**`CreateDefaultScene()` builds a scene but does not upload it.**
`UploadSceneResources()` is the single "make this scene drawable" step — meshes,
lights, debug meshes, the environment's IBL cubemaps, and the light SSBO — and
every part of it skips work that is already cached, so calling it twice is nearly
free. Keeping it in one piece is the point: it used to be two calls, and the
first-launch fallback (whose scene comes from `CreateDefaultScene()` and never
passes through `FinishLoad()`) ran only the first of them. The scene then held an
Environment whose properties read correctly in the Property panel while its
`EnvironmentGPU` never reached the render cache, so the scene rendered unlit.

**Replacing a scene drops the outgoing scene's GPU resources first.**
`DropSceneGpuResources()` (drains the device, then calls
`RenderCache::RemoveSceneResources()`) is invoked by `BeginLoad()` and
`CreateDefaultScene()`. The eviction lives in the cache because only it knows
which of its maps are scene-scoped; the Editor contributes the drain, since the
entries own `vk::raii` resources. This is not housekeeping: the caches are keyed
by object UID and `UID::serialize()` *restores* `o_id` from the file, so an entry
left behind shadows whatever returns under the same id. Load project A, then
project B, and B draws A's geometry, shadow maps and cubemaps while its own
properties are displayed correctly beside them. The same reasoning is why the
environment upload is skip-if-cached while `GenerateIBL()` stays the forced
variant for live changes (`EnvironmentChanged`, an environment re-entering the
scene).

`DeferredRenderer::DrawFrame()` still carries a null-camera precondition check as
defense in depth: it skips the frame and logs once (`m_reportedNoCamera`) instead
of faulting. That guard is a diagnostic, not a license — `ViewCamera()` cannot
return null unless a scene-mutation path broke the contract above, and that path
is the bug. `test/editor/test_editor_camera.cpp` pins the whole contract (GPU-free,
runs in CI): pooled-but-not-scene-content, rebuilt after every pool clear,
identity and pose surviving save/reopen, a camera-less project loading with
nothing injected, deleting every scene camera, and activation taking the view with
deactivation handing it back *at the pose it was left at*.

**`Edit()` pushes the view camera into `EditorViewport` twice per frame**, before
and after `ed_eventBus.Process()`. Activating or deactivating a scene camera — and
deleting the activated one — all land inside `Process()`, and the gizmo rebuild
that follows projects its geometry through this viewport at most once per dirty
flag. Rebuilding against the camera the frame *started* with would leave the handle
at the previous camera's scale and screen position. A third push lives in the
`SceneObjectDeleteRequested` subscription, registered after the controllers so it
runs once the object is actually gone, which is what keeps the pointer from
dangling for the rest of that `Process()`.

### The viewport extent outlives the scene, and reaches both cameras

`HandleResize(logical, renderExtent)` caches both extents in `EditorViewport`
**unconditionally and before** anything camera-related: the extent is a property
of the viewport, not of whichever scene happens to be loaded, and it arrives once
at startup — long before any File → New. A camera seeded later, or restored from a
project saved on a differently sized window, carries an aspect ratio unrelated to
the current viewport (a fresh `Camera` is 1×1), and only a window resize ever
corrected it, so New rendered a squashed frame until the user dragged the window.

It then enqueues a `CameraResizeEvent` for the editor camera **and** a second one
for the activated scene camera, if there is one. Both view candidates are
re-framed, not just the one currently in use: activation can switch the view with
no resize in between, and a camera that missed a resize would present a stretched
image the moment it took over. The events carry a UID and `CameraController`
resolves it through the *pool*, which is exactly why the editor camera is reachable
by the ordinary event despite not being scene content.

`ApplyViewportToViewCamera()` is the same correction applied *directly* rather than
through the bus, because it runs while a scene is still being built (both scene-swap
paths call it, and there is no drain in between). It no-ops while the extent is
still `0×0` (startup, before the window is shown) rather than pushing a degenerate
aspect into the projection. Both branches are pinned by
`test_editor_scene_lifecycle.cpp`;
`test_editor_camera.cpp::ResizeReframesBothTheEditorAndTheActivatedSceneCamera`
pins the two-camera half.

## Core Responsibilities

1. **Event Management** — See `.github/instructions/events.instructions.md` for the
   complete event system: UIEvents singleton, EventQueue typed dispatcher, event
   structs, and the `ConnectUIEvent`/`OnUIEvent` template forwarding pattern.

2. **Context Provisioning**
   - `EditorContext` aggregates scene state and editor state
   - Provides immutable data to Renderer
   - Updated by Editor logic, read by other layers

3. **Controller Orchestration**
   - `Controllers` base class: `virtual Init(ControllerContext& ctx)` binds the controller to the controller context
   - `Editor::RegisterController<T>()` — template factory that creates controller, calls `Init(m_ctx)`, stores in `ed_controllers`
   - **`ControllerContext`** (`src/editor/controllers/ControllerContext.h`) — the ONLY thing a controller depends on: it bundles the three controller-facing interfaces (`IEventQueue&` for subscribe/enqueue/emitNow, `IResourceLookup&` for pooled-object lookup by id, `IOperationSink&` for recording undoable operations) plus providers for the two Editor-owned singletons that are NOT pooled UID objects (`scene` — the current Scene, re-queried per use because New/Load swaps it; `config` — the live RenderConfig). Controllers MUST NOT store the context or any of its members; handler lambdas capture it by value.
   - `CameraController` — event-driven orbit/zoom/dolly/pan via `CameraEvents` (rotate, push, slide, zoom); events carry `int camId`, which `ResolveCamera` resolves against the **resource pool**, not `cam_list`, so the editor camera (pooled, never scene content) is manipulated by the same events as any scene camera
   - `SceneController` — event-driven scene mutations (selection, transform, visibility, camera/mesh/light/env/debug property edits, scene membership add/delete); stateless with free-function handlers in the .cpp; each handler resolves the event's `int objectUid` against the current Scene (typed pool lookup) and mutates the object directly; emits `EditorEvents` (SceneModified, LightGpuChanged, LightingRebuild, RenderResetEvent) for GPU uploads and dirty tracking; see events.instructions.md for the GPU-sync flow
   - **Import/Add split**: `Editor::OnMeshImport`/`OnCameraAdd`/`OnLightAdd`/... only LOAD the resource into the pool and forward the object UID via `SceneObjectAddRequested`; the SceneController fetches the pooled object by UID, registers it, selects it, and records `CompositeOp[SceneObjectAddOp({u},true), SetSelectionOp(...)]`. The Delete gesture (`ObjectDeleteRequested` - the Editor wraps the UI's `DeleteRequested` intent) is **forward-only**: the SceneController snapshots the selection, deselects, then DEFERS the removals as ONE batched `SceneObjectDeleteRequested` carrying all selected UIDs — so the batched handler is the single removal path shared with replay (no replay-only handling) — and records `CompositeOp[SetSelectionOp(before→∅), SceneObjectAddOp(uids,false)]` (delete of N = one op). Light membership changes enqueue `LightingRebuild` (the SSBO is a scene projection). GPU caches (MeshGPU, shadow maps) are scene-scoped: `UploadSceneResources` uploads only objects present at load, and an object entering the scene (live add or undo/redo replay) or a light whose shadow was just enabled enqueues `SceneObjectGpuUploadRequested`, which the Editor resolves with an on-demand upload (skip-if-cached).
   - `ShaderController` — event-driven shader lifecycle via `ShaderEvents` (create, compile, code/struct edit, field add); enqueues `RenderResetEvent` after create/compile so temporal accumulation resets
   - `TransformGizmoController` — event-driven modal transform via `GizmoEvents` (arm, constrain, drag, confirm, cancel); owns nothing, calls exactly one `TransformGizmo` method per event, and is registered **after** `CameraController` so a drag handler sees the camera pose this frame's orbit already produced. The drag writes `Transform3D` directly and submits nothing; confirm submits exactly **one** `SetPositionOp`/`SetRotationOp`/`SetScaleOp` for the whole gesture, so it must emit `SceneModified` + `RenderResetEvent` itself (and `LightGpuChanged` for a light, whose GPU position lives in an SSBO the transform write does not touch)
   - **Pure-intent wrapping**: the Editor subscribes to the UI's `ObjectClicked` and `DeleteRequested` intents and forwards the dedicated scene events `ObjectSelected{ objectUid, modifiers }` and `ObjectDeleteRequested{}` (the two wrap subscriptions in `Editor::Initialize`). Panels never stamp the scene.
   - Controllers receive discrete events (not per-frame polling); `Editor::Edit()` dispatches all enqueued events via `EventQueue::Process()`

4. **GPU Upload & Asset Import**
   - Editor handles asset import events (mesh, camera, light adds) and performs GPU uploads directly (mesh upload, light SSBO dict update, IBL generation)
   - Editor subscribes to cross-component `EditorEvents` (LightGpuChanged, LightingRebuild, SceneModified) emitted by `SceneController` and executes GPU uploads against its `DeferredRenderer`/`UploadManager`
   - Must NOT directly mutate GPU resources outside of event handlers — scene mutations go through `SceneController`

## Key Components

### EditorContext

```cpp
class EditorContext : public QObject {
    Q_OBJECT
public:
    explicit EditorContext(QObject* parent = nullptr);

    // Scene state (immutable view for Renderer)

    // Editor state

    // Selection state (future)

private:
    // Scene graph (future)
    // Selection manager (future)
};
```

**Design:**
- Pure data class - holds state, no rendering logic
- QObject subclass for signal/slot and QML exposure
- Non-copyable
- RAII - constructor fully initializes, destructor cleans up

### Controllers (Base Class)

```cpp
class Controllers
{
public:
    virtual ~Controllers() = default;
    virtual void Init(ControllerContext& ctx) = 0;
};
```

**Design:**
- Pure virtual base class for all editor controllers
- `Init(ControllerContext& ctx)` receives the controller context (event dispatch, pooled-object lookup, operation sink, scene/config providers)
- Derived classes subscribe to typed events in `Init()` (e.g. `ctx.events.subscribe<CameraEvents>()`); handler lambdas capture the context BY VALUE so they are fully self-contained
- Stored via `std::unique_ptr<Controllers>` in Editor's controller list

### Editor::RegisterController<T>()

```cpp
template<typename T>
void Editor::RegisterController()
{
    auto ctrl = std::make_unique<T>();
    ctrl->Init(m_ctx);
    ed_controllers.push_back(std::move(ctrl));
}
```

**Design:**
- Template factory: creates controller, calls `Init(m_ctx)`, stores ownership
- The context carries everything a controller needs — event dispatch, pool lookup, operation sink, scene, and render config — so ALL controllers (including SceneController and RenderConfigController) are now registered uniformly via `RegisterController<T>()`; no providers are constructed manually (see `Editor::Initialize`).
- Controllers are event-driven — no per-frame polling required

### Editor::Edit() — Event Dispatch

`Editor::Edit()` dispatches all enqueued events through the EventQueue. Input
translation (mouse events → camera events) is handled in `Editor::Initialize()`
via `MouseMoveEvent` / `MouseScrollEvent` subscriptions, so `Edit()` is a pure
`EventQueue::Process()` call:

```
Editor::Edit()
  └── EventQueue::Process()
        ├── CameraController handles each event
        ├── SceneController handles each event
        └── etc.
```

### DebugDrawBuilder (issue #22)

`DebugDrawBuilder` is the **only** place that walks `Scene::dLine_list`,
`dPoints_list` and `dMesh_list` and flattens them into the `DebugDrawList` that
`EditorContext::debugDraw` publishes to `DebugPass`. It is a plain member of
`Editor` (`m_debugDraw`), not a controller: it has no events of its own.

**Lifecycle — dirty-flagged, never per-frame polling:**

```
RenderResetEvent  ──► m_debugDraw.MarkDirty()      // O(1), the existing
                                                    // "something visible changed" broadcast
Editor::Edit()
  └── EventQueue::Process()
  └── m_debugDraw.Rebuild(*m_scene)                 // after the queue drains, so a
                                                    // scene change and its overlay
                                                    // consequences land in one frame
```

`Rebuild()` returns immediately when clean, which is the common case — debug objects
are **stateful** and rarely move. `m_dirty` starts `true` so the first `Edit()`
populates the list. `BeginLoad()` / `FinishLoad()` / `NewScene()` also `MarkDirty()`,
because a scene swap invalidates the whole flattened overlay.

**Four contracts the renderer trusts and no GPU test can check** (all pinned by
`test/editor/test_debug_draw_builder.cpp`):

1. **Partition.** Every depth-tested primitive precedes every x-ray one;
   `xraySegmentStart` / `xrayPointStart` mark the boundary exactly. X-ray primitives
   are staged in member scratch vectors (`m_xraySegments` / `m_xrayPoints` — members
   so their capacity survives across frames, cleared at the top of every `Rebuild`)
   and concatenated at the end, so **no sort is ever needed**. `DebugPass` turns
   those two integers straight into draw ranges, so an off-by-one draws x-ray
   geometry with the depth test still on while every draw and pixel count still
   looks plausible. Wire meshes are deliberately **not** partitioned — there are few
   of them and `DebugPass` already switches pipeline per mesh.
2. **Revision.** Exactly **one `Touch()` per rebuild**. `DebugDrawList::Clear()`
   deliberately does *not* Touch, so a clear+refill counts as one revision. A
   missing Touch freezes the overlay; a double Touch re-uploads every frame.
3. **World-space baking.** `DebugDrawList` carries no matrix for segments or
   sprites, so the builder folds each object's `GetModelMatrix()` in while copying.
   Only `DebugWireMesh` keeps a `model` matrix, because its geometry is never copied
   — `DebugPass` draws it straight from the mesh's MeshGPU with the transform in a
   push constant.
4. **Visibility filtering.** A hidden object is *not flattened*. `DebugDrawList` has
   no per-primitive enable bit — `DebugPass` draws whole ranges — so the only way to
   hide a debug object is to leave its primitives out of the list, and the filter has
   to run **before** the x-ray partition so the two boundary integers count only
   surviving primitives. `IsVisible()` ANDs `is_viewport` and `is_rendered`, the same
   way `GeometryPass`, `ShadowDepthPass` and `UploadManager` AND them: the overlay is
   editor-only so `is_viewport` is the flag that obviously applies, but honouring only
   one of the pair would make the Outliner's two toggles mean different things for a
   debug row than for a mesh row. Nothing extra is needed to *notice* a toggle —
   `SceneController::OnVisibilityChanged` already calls `Mutated()`, which enqueues
   `RenderResetEvent` → `MarkDirty()`, and undo replays the same event through
   `SetVisibilityOp`.

**Flattening rules:**

- `DebugLine` vertices are **endpoint pairs**; a trailing odd vertex is dropped and
  a lone vertex emits nothing.
- **Smoothing is derived, never authored, and on by default.** The Scene stores no
  smooth bit, `DebugProperties` shows no checkbox, and there is no event or op for it —
  the builder decides per primitive kind:
  - solid `DebugLine` → `Smooth`; stippled → not
    (`flags |= GetStipple() ? Stipple : Smooth`, so the two are never both set).
    Antialiasing fades alpha towards the quad edge, which would soften the hard ends a
    dash is made of.
  - point sprites → always `Smooth`, in both projection modes. `DebugPoints` has no
    line style to protect, and `overlay_point.frag`'s shape mask is an analytic distance
    (Chebyshev / Manhattan / Euclidean), so a rhombus or circle boundary is visibly
    stepped without the fade.
  - CUBE edges → `Smooth`, like any solid segment.
  - `DebugMesh` wireframes → **never** `Smooth`. They rasterize through
    `PolygonMode::eLine`, which hands the fragment stage no distance-to-edge, so
    `debug_wire.frag` has no branch on the flag; setting it would advertise a
    behaviour no consumer implements.
- Opacity is folded into the packed alpha (`PackTinted`), because the GPU only ever
  sees one alpha — opacity is an authoring convenience, not a second channel.
- `DebugPoints::PointType::CUBE` has **no sprite form** (a screen-aligned sprite
  cannot show a cube's orientation), so it decomposes into 12 axis-aligned wireframe
  segments of width 1.0 with half extent `GetScale() * 0.5f`. `ScreenSpaceSize` is
  deliberately *not* set on them: their size is world-space by definition.
- `ScreenSpaceSize` is set for sprites when `GetProjectionMode() == 0`. It is
  meaningless for segments — segment width is always pixels.
- A `DebugMesh` without geometry (`!HasGeometry()`) is skipped: the wireframe is
  drawn from its MeshGPU, which does not exist until the geometry is uploaded.

> **Ordering caveat for tests and callers:** `Scene::ResPool` is an
> `unordered_map`, so the order of primitives coming from *different* objects is
> unspecified. Never depend on an absolute index — assert over counts, boundary
> values, or predicates keyed off the boundary itself.

**MeshGPU upload for `DebugMesh`.** A wireframe needs vertex/index buffers in the
`RenderCache`, keyed by the object id `DebugWireMesh::meshObjectId` carries — without
them `DebugPass` silently skips the mesh, so this is the difference between a wired
feature and an inert one. `DebugMesh` derives from `ObjectID + Transform3D`, **not**
from `Mesh`, so `UploadManager::UploadMesh(const Mesh&)` cannot serve it by upcast;
the geometry half is split out as `UploadManager::UploadMeshData(const MeshData&)`
and both paths share it. Three places must stay in step:

| Site | Role |
|------|------|
| `UploadDebugMeshGpu()` (Editor.cpp anon namespace) | the single upload path, skip-if-cached, mirroring `UploadMeshGpu()` |
| `Editor::UploadSceneResources()` | `dMesh_list` loop, for objects present at project load |
| `Editor::OnSceneObjectGpuUpload()` | `Get<DebugMesh>` branch, for on-demand upload of an object entering the scene |

`ResourceComponent::Load` needs a matching `ForEach<DebugMesh>` block to re-wire
`o_mesh` from `o_meshDataId`: the `ForEach<Mesh>` block above it does not reach a
`DebugMesh`, so without it a reloaded project leaves the wireframe geometry-less and
the upload is skipped for a reason that looks like a rendering bug.

### TransformGizmo + GizmoDrawBuilder (modal transform)

Blender-style modal transform: `G` / `R` / `S` arm a gesture on the **active object**,
`X` / `Y` / `Z` constrain it, `W` drops back to screen space, LMB or Return confirms,
Esc / RMB / viewport focus loss cancels. Two Editor-owned members, split by
responsibility and both Qt-free and Vulkan-free:

| Member | Role |
|---|---|
| `m_gizmo` (`TransformGizmo`) | the state machine: what is armed, on what, since what snapshot |
| `m_gizmoDraw` (`GizmoDrawBuilder`) | the picture: state + viewport → `GizmoDrawList` |

Neither is a controller (`TransformGizmoController` is, and owns nothing). The gizmo
is **not** a Scene object and must not reuse the Debug objects: its geometry is
derived every rebuild from gesture state, is never serialized, never selectable, and
never appears in the Outliner.

**Every drag derives from the snapshot, never from last frame's value.** `Arm()` takes
`m_before` once; `Drag()` computes the transform as a function of `(m_before, cursor)`
alone. Accumulating deltas would drift by construction and would make a cursor round
trip land near — not on — the starting value. Because nothing accumulates, a return to
the anchor pixel reproduces `m_before` *bitwise*, which is what
`test_transform_gizmo.cpp` asserts with `==` rather than a tolerance.

**Re-constraining normalizes back to the before-state, and that is the whole of W.**
`Constrain()` re-latches its anchors at the current cursor and the controller then
issues one `Drag()` at that same cursor, which must reproduce `m_before` exactly. An
axis switch therefore *undoes* the previous axis's partial transform with no special
case, and W is just `GizmoAxis::None` travelling the same path.

**Two axis sets, because the model matrix is `T * Rz(yaw) * Rx(pitch) * Ry(roll) * S`.**
Rotate uses the gimbal set (`Z → world Z`, `X → Rz·X`, `Y → Rz·Rx·Y`), so a
constrained rotation's PropertyPanel number stays exactly `before + theta` with no
matrix round trip. Move and Scale use the true local set (the columns of `R`). The two
share Y always, X only when `roll == 0`, and Z only when pitch *and* roll are zero — a
pure pitch already separates the two Z axes. Both are derived from the stored Euler
triple, never from `mat3(model)`'s columns, which carry scale and would hand back a
zero axis on a zero scale component and a flipped one on a mirrored object.

A **free** rotate turns about the view axis and is composed as `R_view(θ) · R_before`,
then read back through `GizmoEulerFromRotation` (the ZXY extraction). That extraction
returns the *canonical* representative, so a stored yaw of 200° comes back as −160° —
the same matrix, different numbers in the panel. Hence the zero-angle case writes
`m_before.rotation` back rather than returning early: an unturned gesture must not
silently canonicalise the triple.

**Guide geometry is a fixed pixel budget, so the camera is an input.** Every length in
`GizmoDrawBuilder` is a constant in logical pixels divided by
`PixelsPerWorldUnit(vp, depth)`, which is what holds the handle's apparent size as the
camera dollies — and is why a camera change must `MarkDirty()` even though the state
machine has not moved. `Rebuild()` early-returns when clean and the builder starts
dirty. It runs in `Edit()` **after** the queue drains and after the second
`SetCamera(ViewCamera())` push, for exactly that reason.

**The anchor is the live transform; only part of it is followed.** `Editor::GizmoAnchor`
resolves the *gesture's own* uid while one is armed (the Outliner can change the
selection mid-gesture, and following that would tear the guide off the object being
transformed) and the selection's active object otherwise. The builder then decides what
to honour: the **pivot rides the live position**, so a Move drags its own handle along,
but the **rotation does not** — guides that spun with the object would destroy the
reference the rotation is being measured against. An armed gesture whose target was
deleted mid-drag falls back to its frozen snapshot rather than blinking out.

**Empty, never null.** The Editor publishes `&m_gizmoDraw.List()` unconditionally;
"nothing armed and nothing selected" is an empty list, and `GizmoPass` early-outs on
null and empty alike. Counts are the only handle a CPU test has on which mode is live
(a Move guide is 9 segments, a Scale guide 1 + 2 sprites, a Rotate ring 48 chords), and
`test_gizmo_draw_builder.cpp` asserts over counts, predicates and the *longest* segment
— never an index, because primitive order is an implementation detail.

**Dimming is RGB, never alpha** (`kCandidateDim`): a translucent guide washes out over
bright geometry. The only alpha the gizmo uses is `OverlayFlag::Smooth`'s analytic edge
fade, which is antialiasing rather than transparency — so every primitive in every state
is fully opaque.

**Draggable handles are a sanctioned future feature**, not an excluded one. The pieces
it needs are already in place: `EditorViewport::Project` for a pixel distance-to-segment
hit test (CPU-analytic, no GPU handle-ID buffer), press precedence ahead of the IDBuffer
pick, and drag-and-release mapped onto the existing confirm.



```cpp
void CameraController::Init(ControllerContext& ctx)
{
    ctx.events.subscribe<CameraZoomEvent>([ctx](const CameraZoomEvent& e) { Zoom(e, ctx); });
    ctx.events.subscribe<CameraRotateEvent>([ctx](const CameraRotateEvent& e) { Orbit(e, ctx); });
    ctx.events.subscribe<CameraPushEvent>([ctx](const CameraPushEvent& e) { Dolly(e, ctx); });
    ctx.events.subscribe<CameraSlideEvent>([ctx](const CameraSlideEvent& e) { Pan(e, ctx); });
}
```

**Design:**
- Event-driven: no per-frame `Update()` polling needed
- Receives discrete camera events (rotate, zoom, push, slide) from Editor
- Each event carries the camera's integer UID (`camId`) + delta magnitude; the handler resolves it against the current scene's `cam_list` via the context
- Does not own the camera — resolves the camera by UID per event
- Located in `src/editor/controllers/CameraController.h`

### SceneController (Event-Driven)

```cpp
void SceneController::Init(ControllerContext& ctx)
{
    // Selection, visibility, transform, camera/mesh/light/env/debug property
    // events, scene membership (32 subscriptions total; see SceneEvents.h)
}
```

**Design:**
- Stateless: all handlers are free functions in an anonymous namespace
- Events carry `int objectUid`; handlers resolve the UID against the current
  Scene (typed pool lookup, e.g. `mesh_list.find(objectUid)`) via the
  ControllerContext and mutate the object directly
- Scene-owned state (selection, membership) is reached through the context's
  scene provider — events never carry the scene
- GPU uploads delegated to Editor via EditorEvents (`LightGpuChanged`,
  `LightingRebuild`, `SceneModified`, `RenderResetEvent`) which Editor
  subscribes to and executes against its `DeferredRenderer`/`UploadManager`
- **Debug property handlers (issue #22)** go through one `ForDebugObject(scene,
  uid, fn)` helper that probes `dLine_list`, `dPoints_list`, `dMesh_list` in turn
  and invokes a generic lambda on the first hit — the three debug classes share
  no base, so the lambda is `auto&` and only compiles against members all three
  have (color, opacity, x-ray) or is used from a type-specific handler. It
  returns whether a pool held the UID, so an unknown UID mutates nothing, records
  nothing and skips `Mutated()`. Each handler captures the before value inside
  the lambda and submits its `TransitionOp` there, so undo is per-property.
- Located in `src/editor/controllers/SceneController.h` / `.cpp`

### ShaderController (Event-Driven)

```cpp
void ShaderController::Init(ControllerContext& ctx)
{
    // Lifecycle (non-undoable): bump Shader::m_version + reset accumulation.
    // (Shader CREATE is handled by the Editor, which constructs the pooled
    // RenderShader and records a ShaderLinkOp - see the Undo/redo controllers
    // section. ShaderController keeps only pool-free handlers.)
    ctx.events.subscribe<ShaderCompileRequested>( [ctx](const ShaderCompileRequested& e) {
        OnCompileShader(e, ctx);
        ctx.events.enqueue(RenderResetEvent{});  // pipeline rebuilt -> reset accumulation
    });

    // Code edits apply live; recording is bracketed by ShaderEditBegin/End so a
    // keystroke burst collapses to ONE undo entry on focus-out.
    ctx.events.subscribe<ShaderCodeEdited>( [ctx](const ShaderCodeEdited& e) { OnCodeEdited(e, ctx); });
    ctx.events.subscribe<ShaderEditBegin>( /* capture m_beforeCode + m_editObjectId */ );
    ctx.events.subscribe<ShaderEditEnd>( /* record SetShaderCodeOp if code changed */ );

    // Discrete struct/field edits: snapshot the element, apply the edit, record one delta op each.
    ctx.events.subscribe<ShaderStructEdited>( /* VisitElement + ApplyFieldEdit; Submit SetShaderFieldOp if before != after */ );
    ctx.events.subscribe<ShaderFieldAdded>( /* AppendDefault + Submit AddShaderFieldOp */ );

    // Undo/redo replay: re-apply one edit dimension + bump version (panel refresh).
    ctx.events.subscribe<ShaderCodeRestored>(     [ctx](const ShaderCodeRestored& e)     { OnCodeRestored(e, ctx); });
    ctx.events.subscribe<ShaderFieldRestored>(    [ctx](const ShaderFieldRestored& e)    { OnFieldRestored(e, ctx); });
    ctx.events.subscribe<ShaderFieldAddRestored>( [ctx](const ShaderFieldAddRestored& e) { OnFieldAddRestored(e, ctx); });
    ctx.events.subscribe<ShaderFieldRemoved>(     [ctx](const ShaderFieldRemoved& e)     { OnFieldRemoved(e, ctx); });
}
```

**Design:**
- Mutation handlers are free functions in an anonymous namespace; gesture state
  (`m_codeEditing`, `m_editObjectId`, `m_editStage`, `m_beforeCode`) lives on
  the controller so begin/end can bracket a burst
- Handlers resolve the event's `int objectUid` to `Mesh*` (via the current
  scene's `mesh_list`, the only shader-owning object type) and mutate
  `mesh->o_shader` data directly — no Editor, Renderer, or GPU state
- Create/Compile bump `Shader::m_version` on success (pipeline rebuild). Create
  is **undoable** (the Editor records `ShaderLinkOp` - undo drops the pooled
  reference, redo relinks it via `ShaderLinkRestored`/`ShaderUnlinkRestored`);
  Compile stays a **non-undoable** lifecycle
  action. Content edits (code/struct/field) only mutate CPU IR/code and require
  the user to press Compile to reach the GPU
- Only Create/Compile enqueue `RenderResetEvent` (temporal accumulation reset)
- Undo/redo of content edits is CPU-only: `OnRestoreSource` overwrites the stage's
  `code` + `parsed` IR and bumps `ShaderUnit::m_version` (panel refresh) — it does
  **not** recompile to SPIR-V (see Undo/Redo controllers below)
- Located in `src/editor/controllers/ShaderController.h`

## Data Flow

```
User Input (QML) → UIEvents Signal → Editor Subscriber → State Update
                                                              ↓
                                                       EventQueue.enqueue
                                                              ↓
                                                       EventQueue.Process()
                                                              ↓
                                                       UI Refresh
```

**3-line newFrame render loop:**
```
UIEvents::newFrame() → Editor::Edit(input) → Renderer::DrawFrame(scene)
```

**Example: Window Resize**
1. QWindow detects resize in MainWindow
2. MainWindow emits `UIEvents::windowResized(w, h)`
3. Renderer slot connected → `Swapchain::Recreate(w, h)`
4. Next `newFrame()` → renders at new resolution

## Architectural Boundaries

### ✅ Editor MAY:
- Mutate scene objects (future)
- Own Controllers and managers
- Emit and subscribe to UIEvents signals and EventQueue typed events
- Update EditorContext state
- Call into scene management systems

### ❌ Editor MUST NOT:
- Directly manipulate GPU resources
- Call Vulkan functions
- Depend on UI implementation details (only UIEvents signals)
- Store rendering-specific state (belongs in Renderer or Data/Resource layer)

## Integration with Other Layers

**With Renderer:**
- Provides scene data via EditorContext
- Receives rendering events via UIEvents (Qt signals)
- Sends Editor events via EventQueue (typed event dispatcher)
- Renderer NEVER calls back into Editor directly
- One-way dependency: Editor → Renderer (via EventQueue)

**With UI:**
- UI emits signals via UIEvents
- Editor subscribes to UI-relevant signals
- UI reads EditorContext for display (future)
- Two-way via UIEvents, NOT direct C++ calls

**With Data & Resource:**
- Editor may request resource creation (e.g., "load this mesh")
- Data layer handles allocation, returns handle
- Editor stores handle, passes to Renderer

## Current Scope (Deferred PBR MVP)

- UIEvents singleton with UI↔Editor signals (newFrame, windowResized, etc.)
- EventQueue typed event dispatcher for Editor↔Renderer events
- EditorContext stub (empty, placeholder for future scene state)
- Editor orchestrator with `RegisterController<T>()` and `Edit()` for input translation
- Controllers base class with `Init(ControllerContext&)` virtual interface (IEventQueue + IResourceLookup + IOperationSink + scene/config providers)
- CameraController: event-driven orbit/zoom/dolly/pan via CameraEvents (int camId payloads)
- Input system: `GetInputState()` returns complete `InputState` for `Edit()` consumption

## Future Enhancements

- Scene loading/saving orchestration
- Transform gizmo interaction

## Undo/Redo controllers (`src/editor/operations/`)

The operation model, coalescing strategies, and history persistence are
documented in
[operation-system.instructions.md](operation-system.instructions.md). This
section covers only how the editor's controllers *produce* operations.

Three controllers use the explicit begin/end gesture pattern (capture "before"
on a begin event, mutate live without recording, record ONE op on the end
event):

- **`CameraController`** — `CameraDragBegin` captures the pose, `CameraRotate/
  Push/Slide` mutate live, `CameraDragEnd` records one `CameraTransformOp`
  (no-op if the pose is unchanged). `CameraTransformOp` is deliberately
  non-mergeable (empty `MergeKey`) so each drag is its own undo entry. Scroll
  zoom has no press/release boundary, so it stays on the implicit-merge path
  via the separate `CameraZoomOp` type (mergeable, keyed `camera_zoom:<uid>`).
  Two op types instead of one boolean flag: the gesture boundary vs. burst
  coalescing are distinct behaviors that belong to distinct ops.
- **`RenderConfigController`** — the single mutation path for the Editor-owned
  `RenderConfig`, reached through the ControllerContext's `config` provider so
  it never includes Editor internals (registered uniformly via
  `RegisterController<T>`). `ConfigEditBegin` captures the config,
  `RenderConfigChangedEvent` applies live, `ConfigEditEnd` records one
  `SetRenderConfigOp`. Discrete edits (checkbox/combo) arrive without a gesture
  and record immediately. No-op writes (`before == after`, via `RenderConfig`'s
  defaulted `operator==`) are never recorded. `SetRenderConfigOp` is scene-level
  (not UID-based) and deliberately non-mergeable (empty `MergeKey`).
- **`ShaderController`** — content edits to a mesh's shader become undoable via
  three fine-grained delta ops, each carrying only its before/after slice (no
  whole-`ShaderStruct` snapshot — keeps history and project files small):
  `SetShaderCodeOp` (before/after GLSL text of one stage), `SetShaderFieldOp`
  (before/after of one whole IR element — a `ShaderFieldValue` variant, keyed by
  section + field index), and `AddShaderFieldOp` (append vs. remove one default entry via a `bool add`
  flag, whose `Inverse()` flips the flag). All are keyed by mesh UID + stage.
  Code edits are gesture-bounded: `ShaderEditBegin` snapshots `m_beforeCode`,
  `ShaderCodeEdited` applies live, `ShaderEditEnd` records one `SetShaderCodeOp`
  on focus-out only if the code changed (no net change → no op). Discrete
  struct/field edits have no gesture: `ShaderStructEdited` snapshots the element,
  applies the UI's `{field,value}` via `VisitElement`/`ApplyFieldEdit`, and records
  a `SetShaderFieldOp` only if the element changed (`before != after`); `ShaderFieldAdded` calls `AppendDefault` and
  records an `AddShaderFieldOp(add=true)`. Undo/redo replays four dedicated
  restore events — `ShaderCodeRestored`, `ShaderFieldRestored`,
  `ShaderFieldAddRestored`, `ShaderFieldRemoved` — distinct from the forward
  events so the replay handlers bump `ShaderUnit::m_version` (panel refresh)
  while live forward edits do NOT (avoids cursor-jump mid-typing). Restore is
  **CPU-only, no recompile to SPIR-V**; the user presses Compile to push
  restored source to the GPU. All three ops are deliberately non-mergeable
  (empty `MergeKey`). Shader Create is undoable via `ShaderLinkOp` (recorded by
  the Editor; a pool-preserving membership toggle - see
  operation-system.instructions.md); Compile stays a non-undoable lifecycle
  action.

