# Controllers

Controllers are the Editor's event-driven reaction layer: each one subscribes to
typed events in `Init()` and mutates state in its handlers, with no per-frame
polling.

## The event-driven pattern

Controllers receive discrete events rather than polling. Only the UI boundary is
deferred: `Editor::Edit()` drains the events a panel signal queued in the previous
frame. Everything else - a controller reaction, a script write, an operation
replay - goes through `Emit`, which runs the handlers before it returns:

```
Editor::Edit()
  |-- viewport camera sync (before the drain)
  |-- EventQueue::Process()        <- the deferred UI events, one frame's worth
        |-- CameraController handles each event
        |-- SceneController handles each event
        |-- etc.
  |-- viewport camera sync again (the drain can change or delete the camera)

ctx.events.Emit(PositionChanged{...})   <- synchronous, anywhere in the editor
  |-- ObjectController::OnPositionChanged runs before Emit returns
```

Input translation (mouse events to camera events) is handled in
`Editor::Initialize()` via `MouseMoveEvent`/`MouseScrollEvent` subscriptions, so
`Edit()` is two viewport-camera syncs around one `EventQueue::Process()` call.

## The base class

```cpp
class Controllers
{
public:
    virtual ~Controllers() = default;
    virtual void Init(ControllerContext& ctx) = 0;
};
```

Design:

- Pure virtual base class for all editor controllers.
- `Init(ControllerContext& ctx)` receives the controller context (event dispatch,
  pooled-object lookup, operation sink, scene/config providers).
- Derived classes subscribe to typed events in `Init()` (for example,
  `ctx.events.Subscribe<CameraZoomEvent>()`); handler lambdas capture the context
  by value so they are fully self-contained.
- Handlers emit through `ctx.events.Emit(...)`, which is synchronous: the event a
  handler emits is fully handled before the handler resumes.
- Stored via `std::unique_ptr<Controllers>` in the Editor's controller list.

## ControllerContext

`ControllerContext` (`src/editor/controllers/ControllerContext.h`) is the only
thing a controller depends on. It bundles the three controller-facing interfaces:

- `IEventQueue& events` for `Subscribe` and synchronous `Emit`
- `IResourceLookup& resources` for pooled-object lookup by id
- `IOperationSink& ops` for recording undoable operations, including the
  `BeginScriptScope` / `EndScriptScope` pair that compacts one script run into
  one entry

plus providers for the Editor-owned singletons that are not pooled UID objects:

- `scene`: the current Scene, re-queried per use because New/Load swaps it
- `config`: the live `RenderConfig`, mutable
- `viewport`: the viewport composition root, mutable

There is one handle per Editor-owned thing, and the viewport is one thing: the
modal transform state is `viewport()->Gizmo()`, not a `gizmo` provider of its
own, because a second provider is a second name for the same object and free to
be wired to a different one.

The viewport handle is mutable only so that `Gizmo()` is reachable; its own
mutators (extent, camera, cursor, overlay dirty flags) stay the Editor's
per-frame job by documented contract, not by a `const` that had to be routed
around. A read-only handler should still bind `const EditorViewport*` locally,
since every query it needs is const.

Ownership rules:

- The context is constructed by the Editor once and outlives the controllers'
  event subscriptions (it is declared before the controller list).
- Controllers MUST NOT store the context or any of its members; handler lambdas
  capture it by value (they outlive the `EventQueue`, whose stored lambdas
  reference it).

## Registration

```cpp
template<typename T>
void Editor::RegisterController()
{
    auto ctrl = std::make_unique<T>();
    ctrl->Init(m_ctx);
    ed_controllers.push_back(std::move(ctrl));
}
```

The context carries everything a controller needs (event dispatch, pool lookup,
operation sink, scene, and render config), so every controller - including
`SceneController`, `ObjectController`, `ScriptController` and
`RenderConfigController` - is registered uniformly through
`RegisterController<T>()`; no providers are constructed manually. Controllers are
event-driven, so no per-frame polling is required.

Registration order matters where two subscribers handle the same event: handlers
run in registration order, and because dispatch is synchronous they also run in
that order within one call. That is why `SceneController`, registered with the
controllers, removes objects before the Editor's own `SceneObjectDeleteRequested`
subscription re-syncs the viewport camera. Where subscribers share no event type,
order is free: `ObjectController` and `SceneController` never see the same event,
and neither does the gizmo with `CameraController` - the gizmo takes
`MouseMoveEvent` and reads the cursor from `e.position`, while `CameraController`
takes the `Camera*` events the Editor derives from it.

## Mutation notification

`neurus::notify` (`src/editor/controllers/MutationNotify.h`) is the single answer
to "what does a scene mutation imply?", shared by every controller. Mutating a
scene object is never just the write: something has to reset temporal
accumulation, something has to refresh the light SSBO the write did not touch,
and something has to mark the project unsaved. Which of those apply is a function
of two things only, the object's type and whether the edit is final, so it is
answered here once instead of at each mutation site.

Two tiers:

- `ObjectMutated(obj, events)` is committed (a panel edit, an add, a confirmed
  gesture) and emits `LightingRebuild` for a light, plus `SceneModified` and
  `RenderResetEvent`.
- `ObjectPreviewed(obj, events)` is transient (one frame of a live drag) and
  emits the cheap `LightGpuChanged` for a light, plus `RenderResetEvent`, and
  deliberately no `SceneModified`, because that flag is one-way: nothing
  un-dirties a project, so a cancelled gesture that had emitted it would leave the
  document needing a save for a change that no longer exists.

The tier asymmetry in light work is deliberate, not an oversight: a preview runs
per mouse-move and buys the single-struct update, while a committed edit pays for
the authoritative full-dict rebuild, since `UploadLighting` filters by shader
variant and visibility while walking `light_list`.

`Mutated`/`LightStructChanged`/`LightingRebuilt`/`Repaint` are the primitives the
two tiers compose, for handlers that already know the type (a light's power edit)
or mutate no object at all (a gizmo guide that appeared or vanished). The type
test itself is `ObjectID::IsLight()`, a member because two of the ten GOTypes
answer yes and every copy of the test is a place to forget the second one.

Scope: this is mutation policy, not a general event facade. A camera orbit and a
render-setting change mutate no scene object and have no tier, so they emit
`RenderResetEvent` directly.

## CameraController

`src/editor/controllers/CameraController.h/cpp` owns event-driven camera
manipulation (orbit/zoom/dolly/pan) via `CameraEvents`.

```cpp
void CameraController::Init(ControllerContext& ctx)
{
    // Gesture boundaries: capture the "before" pose on begin, record one op on end.
    ctx.events.Subscribe<CameraDragBegin>([this, ctx](const CameraDragBegin& e) { /* capture */ });
    ctx.events.Subscribe<CameraDragEnd>([this, ctx](const CameraDragEnd& e) { /* record */ });

    ctx.events.Subscribe<CameraRotateEvent>([ctx](const CameraRotateEvent& e) { OnCameraRotate(e, ctx); });
    ctx.events.Subscribe<CameraPushEvent>([ctx](const CameraPushEvent& e) { OnCameraPush(e, ctx); });
    ctx.events.Subscribe<CameraSlideEvent>([ctx](const CameraSlideEvent& e) { OnCameraSlide(e, ctx); });
    ctx.events.Subscribe<CameraZoomEvent>([ctx](const CameraZoomEvent& e) { OnCameraZoom(e, ctx); });
    ctx.events.Subscribe<CameraResizeEvent>([ctx](const CameraResizeEvent& e) { OnCameraResize(e, ctx); });
}
```

Design:

- Event-driven: no per-frame `Update()` polling needed.
- Receives discrete camera events (rotate, zoom, push, slide) from the Editor.
- Each event carries the camera's integer UID (`camId`) plus a delta magnitude;
  the handler resolves it through `ResolveCamera`. `ResolveCamera` resolves
  against the resource pool, not `cam_list`, so the viewport camera (pooled,
  never scene content) is manipulated by the same events as any scene camera.
- Does not own the camera; resolves it by UID per event.

A `CameraResizeEvent` is emitted on camera activation because a camera restored
on a different window size (or never framed) carries a stale aspect.

## SceneController

`src/editor/controllers/SceneController.h/cpp` owns the scene **document**:
selection, scene membership (add/delete) and camera activation. Object property
writes belong to `ObjectController`.

```cpp
void SceneController::Init(ControllerContext& ctx)
{
    // Selection
    ctx.events.Subscribe<ObjectSelected>(...);
    ctx.events.Subscribe<ObjectDeselected>(...);
    ctx.events.Subscribe<SelectionChanged>(...);
    // Active camera (scene state, independent of selection)
    ctx.events.Subscribe<ActiveCameraChanged>(...);
    // Membership
    ctx.events.Subscribe<SceneObjectAddRequested>(...);
    ctx.events.Subscribe<SceneObjectDeleteRequested>(...);
    ctx.events.Subscribe<ObjectDeleteRequested>(...);
}
```

Seven subscriptions in total; each forwards to a free-function handler.

Design:

- Stateless: all handlers are free functions in an anonymous namespace.
- Events carry `int objectUid`; handlers resolve it against the current Scene
  (typed pool lookup, for example `mesh_list.find(objectUid)`) through the
  context's scene provider and mutate the document directly. The add handler also
  fetches the pooled object by UID, because the pool owns the resource.
- Scene-owned state (selection, membership, active camera) is reached through the
  context's scene provider; events never carry the scene.
- Most mutating handlers finish with a `neurus::notify` call rather than their own
  `Emit`s, so the `EditorEvents` they emit (`SceneModified`, `LightingRebuild`,
  `RenderResetEvent`) cannot drift from the gizmo's. The add handler and the
  active-camera handler also emit their own follow-up events: the add handler
  asks for the GPU upload before it submits its composite, and camera activation
  emits a `CameraResizeEvent` for the camera it just activated.
- GPU uploads are delegated to the Editor via `EditorEvents`
  (`LightGpuChanged`, `LightingRebuild`, `SceneModified`, `RenderResetEvent`,
  `SceneObjectGpuUploadRequested`), which the Editor subscribes to and executes
  against its `DeferredRenderer`/`UploadManager`.

### Import/Add split and the delete path

`Editor::OnMeshImport`/`OnCameraAdd`/`OnLightAdd`/... only load the resource into
the pool and forward the object UID via `SceneObjectAddRequested`. The
`SceneController` fetches the pooled object by UID, registers it, selects it, and
records `CompositeOp[SceneObjectAddOp({u},true), SetSelectionOp(...)]`.

The Delete gesture (`ObjectDeleteRequested`, the Editor wraps the UI's
`DeleteRequested` intent) is forward-only. The `SceneController` snapshots the
selection, deselects, then emits one batched `SceneObjectDeleteRequested`
carrying all selected UIDs, so the batched handler is the single removal path
shared with replay (no replay-only handling), and records
`CompositeOp[SetSelectionOp(before->none), SceneObjectAddOp(uids,false)]` (delete
of N is one op). The emit deliberately precedes the submit: the removal runs
synchronously inside it, while `SceneObjectAddOp` stores only a UID list and a
direction, so the later submit reads no state the removal invalidated.

Light membership changes emit `LightingRebuild` (the SSBO is a scene projection).
GPU caches (`MeshGPU`, shadow maps) are scene-scoped: `UploadSceneResources`
uploads only objects present at load, and an object entering the scene (live add
or undo/redo replay) or a light whose shadow was just enabled emits
`SceneObjectGpuUploadRequested`, which the Editor resolves with an on-demand
upload (skip-if-cached).

### Pure-intent wrapping

The Editor subscribes to the UI's `ObjectClicked` and `DeleteRequested` intents
and forwards the dedicated scene events `ObjectSelected{ objectUid, modifiers }`
and `ObjectDeleteRequested{}` (the two wrap subscriptions in
`Editor::Initialize`). Panels never stamp the scene.

## ObjectController

`src/editor/controllers/ObjectController.h/cpp` owns **every object property
write**: visibility, transforms, and the camera / mesh / light / environment /
debug knobs. That is 26 subscriptions; see `SceneEvents.h` for the structs.

```cpp
void ObjectController::Init(ControllerContext& ctx)
{
    // Visibility, position/rotation/scale, camera target/fov/pose,
    // mesh shadow/material, light power/color/radius/shadow/cutoff,
    // environment intensity/rotation, and every Debug*Changed knob.
    ctx.events.Subscribe<PositionChanged>([ctx](const PositionChanged& e) { OnPositionChanged(e, ctx); });
    // ... one Subscribe per property event
}
```

Design:

- Resolution goes through `IResourceLookup`, never the scene. Writability is a
  property of the pool, not of scene membership, so a pooled object that is not in
  the document - an object held by undo history, or the pooled viewport camera -
  is editable. That is also what lets a script write to an object it holds before
  the object enters the scene.
- It never reads the scene: `test/editor/test_object_controller.cpp` hands it a
  scene provider that calls `ADD_FAILURE()`, so a regression cannot pass silently.
- Stateless: free functions in an anonymous namespace. Most handlers capture the
  before value, mutate through the pool, submit one forward operation through
  `ctx.ops`, and route the consequences through `neurus::notify`.
  `OnCameraPoseChanged` follows the same shape: an absolute pose write applies
  the pose and records a `CameraTransformOp`, and the replay of that op re-enters
  the handler with `Submit` suppressed by the phase guard.
- Debug property handlers go through one `ForDebugObject(resources, uid, fn)`
  helper that probes the `DebugLine`, `DebugPoints` and `DebugMesh` pools in turn
  and invokes a generic lambda on the first hit. The three classes share
  `ObjectID` and `Transform3D`, but no shared base declares color, opacity or
  x-ray, so the lambda is `auto&` and the three pool lookups stay in one place.
  It returns whether a pool held the UID, so an unknown UID mutates nothing,
  records nothing, and skips `Mutated()`. Each handler captures the before value
  inside the lambda and submits its `TransitionOp` there, so undo is per-property.
- Camera target edits fold into the coupled `CameraTransformOp` (position
  unchanged) so a panel target edit and a viewport drag share one undoable
  "Camera Transform"; FOV records a `CameraFovOp` of its own, and an absolute
  pose write records a `CameraTransformOp` - both on the live path (a script
  setting `Camera.position`) and on replay.

## ShaderController

`src/editor/controllers/ShaderController.h/cpp` owns event-driven shader
lifecycle (create, compile, code/struct edit, field add) via `ShaderEvents`.

```cpp
void ShaderController::Init(ControllerContext& ctx)
{
    // Lifecycle (non-undoable): bump Shader::m_version + reset accumulation.
    // (Shader CREATE is handled by the Editor, which constructs the pooled
    // RenderShader and records a ShaderLinkOp - see the Undo/redo section.
    // ShaderController keeps only pool-free handlers.)
    ctx.events.Subscribe<ShaderCompileRequested>( [ctx](const ShaderCompileRequested& e) {
        OnCompileShader(e, ctx);
        ctx.events.Emit(RenderResetEvent{});  // pipeline rebuilt -> reset accumulation
    });

    // Code edits apply live; recording is bracketed by ShaderEditBegin/End so a
    // keystroke burst collapses to ONE undo entry on focus-out.
    ctx.events.Subscribe<ShaderCodeEdited>( [ctx](const ShaderCodeEdited& e) { OnCodeEdited(e, ctx); });
    ctx.events.Subscribe<ShaderEditBegin>( /* capture m_beforeCode + m_editObjectId */ );
    ctx.events.Subscribe<ShaderEditEnd>( /* record SetShaderCodeOp if code changed */ );

    // Discrete struct/field edits: snapshot the element, apply the edit, record one delta op each.
    ctx.events.Subscribe<ShaderStructEdited>( /* VisitElement + ApplyFieldEdit; Submit SetShaderFieldOp if before != after */ );
    ctx.events.Subscribe<ShaderFieldAdded>( /* AppendDefault + Submit AddShaderFieldOp */ );

    // Undo/redo replay: re-apply one edit dimension + bump version (panel refresh).
    ctx.events.Subscribe<ShaderCodeRestored>(     [ctx](const ShaderCodeRestored& e)     { OnCodeRestored(e, ctx); });
    ctx.events.Subscribe<ShaderFieldRestored>(    [ctx](const ShaderFieldRestored& e)    { OnFieldRestored(e, ctx); });
    ctx.events.Subscribe<ShaderFieldAddRestored>( [ctx](const ShaderFieldAddRestored& e) { OnFieldAddRestored(e, ctx); });
    ctx.events.Subscribe<ShaderFieldRemoved>(     [ctx](const ShaderFieldRemoved& e)     { OnFieldRemoved(e, ctx); });
}
```

Design:

- Mutation handlers are free functions in an anonymous namespace; gesture state
  (`m_codeEditing`, `m_editObjectId`, `m_editStage`, `m_beforeCode`) lives on the
  controller so begin/end can bracket a burst.
- Handlers resolve the event's `int objectUid` to `Mesh*` (via the current
  scene's `mesh_list`, the only shader-owning object type) and mutate
  `mesh->o_shader` data directly, with no Editor, Renderer, or GPU state.
- Create/Compile bump `Shader::m_version` on success (pipeline rebuild). Create is
  undoable (the Editor records `ShaderLinkOp`: undo drops the pooled reference,
  redo relinks it via `ShaderLinkRestored`/`ShaderUnlinkRestored`); Compile stays
  a non-undoable lifecycle action. Content edits (code/struct/field) only mutate
  CPU IR/code and require the user to press Compile to reach the GPU.
- Only Create/Compile emit `RenderResetEvent` (temporal accumulation reset).
- Undo/redo of content edits is CPU-only: `OnRestoreSource` overwrites the
  stage's `code` + `parsed` IR and bumps `ShaderUnit::m_version` (panel refresh);
  it does not recompile to SPIR-V. See [Operations](operations.md).

## RenderConfigController

`RenderConfigController` is the single mutation path for the Editor-owned
`RenderConfig`, reached through the `ControllerContext`'s `config` provider so it
never includes Editor internals (registered uniformly via
`RegisterController<T>`). `ConfigEditBegin` captures the config,
`RenderConfigChangedEvent` applies live, and `ConfigEditEnd` records one
`SetRenderConfigOp`. Discrete edits (checkbox/combo) arrive without a gesture and
record immediately. No-op writes (`before == after`, via `RenderConfig`'s
defaulted `operator==`) are never recorded. `SetRenderConfigOp` is scene-level
(not UID-based) and deliberately non-mergeable (empty `MergeKey`).

## ScriptController

`ScriptController` is the stateless bridge between a script run and the operation
sink. It subscribes to two events and does nothing else:

```cpp
void ScriptController::Init(ControllerContext& ctx)
{
    ctx.events.Subscribe<ScriptRunStarted>([ctx](const ScriptRunStarted& e) {
        ctx.ops.BeginScriptScope(e.name);
    });
    ctx.events.Subscribe<ScriptRunFinished>([ctx](const ScriptRunFinished&) {
        ctx.ops.EndScriptScope();
    });
}
```

`e.name` is the History label the host chose - "Run Script" for a console run,
"Run <file>.py" for a file run.

`ScriptEngineImpl::RunSource` emits `ScriptRunStarted` before it executes the
source and `ScriptRunFinished` on every exit path, so a raising run still closes
its scope. The scope itself is the operation manager's; see
[Operations](operations.md#one-undo-step-per-script-run).

## TransformGizmoController

`TransformGizmoController` owns the event-driven modal transform via
`GizmoEvents` (arm, constrain, drag, confirm, cancel). It owns nothing and calls
exactly one `TransformGizmo` method per event; it shares no event type with
`CameraController`, and it reads the drag cursor from the event's `position`
rather than from the viewport, so its registration order next to
`CameraController` has no dispatch consequence. The drag writes `Transform3D`
directly and submits nothing; confirm submits exactly one
`SetPositionOp`/`SetRotationOp`/`SetScaleOp` for the whole gesture. Because the
ops are not what notifies, the controller notifies
itself through the shared `neurus::notify` tiers above, never its own `Emit`s:
every drag frame (and the cancel path, which restores the snapshot) is
`ObjectPreviewed`, a confirm is `ObjectMutated`, and a guide that merely appeared
or vanished is `Repaint`. That is what makes a whole G-drag-Esc round trip leave
no trace, and `test/editor/test_transform_gizmo_controller.cpp` pins it (the
tiers are invisible in a screenshot and wrong only much later). For the state
machine and guide geometry, see [Transform gizmo](gizmo.md).

## Producing operations

The operation model, coalescing strategies, and history persistence are
documented in [Operations](operations.md). This section covers only how the
controllers produce operations.

Three controllers use the explicit begin/end gesture pattern (capture "before" on
a begin event, mutate live without recording, record one op on the end event):

- `CameraController`: `CameraDragBegin` captures the pose,
  `CameraRotate`/`CameraPush`/`CameraSlide` mutate live, `CameraDragEnd` records
  one `CameraTransformOp` (no-op if the pose is unchanged). `CameraTransformOp` is
  deliberately non-mergeable (empty `MergeKey`) so each drag is its own undo
  entry. Scroll zoom has no press/release boundary, so it stays on the
  implicit-merge path via the separate `CameraZoomOp` type (mergeable, keyed
  `camera_zoom:<uid>`). Two op types instead of one boolean flag: the gesture
  boundary vs. burst coalescing are distinct behaviors that belong to distinct
  ops.
- `RenderConfigController`: described above.
- `ShaderController`: content edits to a mesh's shader become undoable via three
  fine-grained delta ops, each carrying only its before/after slice (no
  whole-`ShaderStruct` snapshot, which keeps history and project files small):
  `SetShaderCodeOp` (before/after GLSL text of one stage), `SetShaderFieldOp`
  (before/after of one whole IR element, a `ShaderFieldValue` variant, keyed by
  section + field index), and `AddShaderFieldOp` (append vs. remove one default
  entry via a `bool add` flag, whose `Inverse()` flips the flag). All are keyed by
  mesh UID + stage. Code edits are gesture-bounded: `ShaderEditBegin` snapshots
  `m_beforeCode`, `ShaderCodeEdited` applies live, `ShaderEditEnd` records one
  `SetShaderCodeOp` on focus-out only if the code changed. Discrete struct/field
  edits have no gesture: `ShaderStructEdited` snapshots the element, applies the
  UI's `{field,value}` via `VisitElement`/`ApplyFieldEdit`, and records a
  `SetShaderFieldOp` only if the element changed (`before != after`);
  `ShaderFieldAdded` calls `AppendDefault` and records an
  `AddShaderFieldOp(add=true)`. Undo/redo replays four dedicated restore events
  (`ShaderCodeRestored`, `ShaderFieldRestored`, `ShaderFieldAddRestored`,
  `ShaderFieldRemoved`) distinct from the forward events, so the replay handlers
  bump `ShaderUnit::m_version` (panel refresh) while live forward edits do not
  (avoiding a cursor jump mid-typing). Restore is CPU-only, no recompile to
  SPIR-V; the user presses Compile to push restored source to the GPU. All three
  ops are deliberately non-mergeable (empty `MergeKey`). Shader Create is
  undoable via `ShaderLinkOp` (recorded by the Editor; a pool-preserving
  membership toggle); Compile stays a non-undoable lifecycle action.

Every controller records through `ctx.ops.Submit`, whether the edit came from a
panel, a gesture or a script. While a script run is open, `OperationManager`
buffers those submits instead of pushing them, so the whole run lands as one
`CompositeOp`; see
[Operations](operations.md#one-undo-step-per-script-run).
