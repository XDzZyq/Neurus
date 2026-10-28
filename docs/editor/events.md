# Events

Neurus's event system enforces layer isolation with a two-tier design: UI panels
emit Qt signals, a bridge in Application converts them to typed event structs, and
the Editor handles them through an internal `EventQueue`.

## Overview

UI panels emit Qt signals; a template bridge in Application converts these to
typed event structs; the Editor subscribes and handles them through an internal
`EventQueue`. The result: panels never include Editor headers, the Editor has
zero Qt dependencies, and all cross-layer wiring is auditable in one file.

```
Panel Qt signal (e.g. Outliner::objectClicked - pure input intent)
  |-- Application::ConnectUIEvent<Outliner, ObjectClicked>
        |-- [lambda calls Editor::OnUIEvent(e)]
              |-- EventQueue::Enqueue<ObjectClicked>   (the only deferred step)
                    |-- EventQueue::Process() in the next Edit()
                          |-- handlers run; every Emit they make runs here too
```

The deferral exists only at the UI boundary. Everything the Editor or a
controller emits dispatches synchronously, so an event and its consequences are
complete before the emitting call returns.

### ID-carrying events

Every event in Neurus is ephemeral: emitted synchronously, it is fully handled and
destroyed before the emitting call returns. A UI signal's event is the one that
waits - it is queued by `Enqueue`, dispatched by the next frame's `Edit()`, and
destroyed with the dispatch. Events carry plain integer object UIDs (never raw
`UID*`/`Camera*` pointers). A raw pointer can dangle after
the object is deleted, while a UID is a stable identity that controllers resolve
against the current scene/pool at dispatch time. `UID` is the common base of
`Scene` and `ObjectID`, so the same integer space covers both. Object property
handlers resolve an event's `int objectUid` through `IResourceLookup` (the pool);
shader and scene handlers resolve it against the current Scene; both come from the
`ControllerContext`. See [Controllers](controllers.md). The scene itself is not
carried in events: scene-scoped handlers obtain the current Scene from the
context, so a scene swap on New/Load can never leave an event holding a stale
pointer. This id-based model is what makes pointer-free event payloads safe across
frames, serialization, and undo/redo replay.

### Dispatch paths

Two paths exist, and they differ only in **when** dispatch happens:

1. **Deferred: the UI boundary.** `Editor::OnUIEvent` calls
   `EventQueue::Enqueue`, which stores a thunk. `Editor::Edit()` calls
   `Process()` once per frame, which pops the queue in FIFO order and dispatches
   each event. `Enqueue` exists on the concrete `EventQueue` only - `IEventQueue`,
   the surface controllers and scripts see, offers no way to defer.
2. **Synchronous: everything else.** Every other production emit goes through
   `IEventQueue::Emit`, which runs every handler for that type before it returns.
   Dispatch is depth-first: an event a handler emits completes before that
   handler resumes, so the caller observes the whole consequence chain, not just
   its first hop.

Depth-first dispatch turns a handler cycle from a bounded queue loop into
recursion, so `EventQueue::Dispatch` counts live frames and refuses at
`kMaxDispatchDepth` (64): it logs `NEURUS_ERR` naming the event type and drops
the event, without invoking handlers. `Process()` saves the entry depth, zeroes it
for the drain and restores it afterwards, so one runaway chain cannot poison a
later frame.

### Ordering consequences

Dispatch order is registration order within a type. Two places depend on it, and
both hold because their subscribers sit in the same `Dispatch` call:

- `SceneObjectDeleteRequested`: `SceneController` removes the objects first, then
  the Editor's own subscription re-syncs the viewport camera, in the same
  dispatch. Without that order the viewport would hold a dangling pointer until
  the next `Edit()`.
- `ObjectSelected`, `ObjectDeselected` and `SelectionChanged`: `SceneController`
  records the operation, then the Editor marks the gizmo dirty.

One pair that shares an event is deliberately order-independent:
`TransformGizmoController` handles `MouseMoveEvent` before the Editor does, and
reads the cursor from `e.position` rather than from `EditorViewport::Cursor()`, so
it does not matter that the Editor has not pushed the cursor into the viewport
yet. `CameraController` shares no event type with the gizmo at all - the gizmo
takes `MouseMoveEvent`, the camera controller the `Camera*` events the Editor
derives from it - so their registration order has no dispatch consequence either.

A separate consequence of synchronous dispatch is that GPU uploads run inside the
handler that mutated the object rather than later in a drain. The queue never
deduplicated, so the amount of work is unchanged; only its interleaving is.
Notification chains terminate (a `RenderResetEvent`, `SceneModified` or
`LightGpuChanged` handler emits nothing), which is why the depth guard is a
backstop rather than a routine outcome.

### Event domains

The three groups below differ in **who wraps** the event and **where it is
resolved**, not in when it is dispatched:

1. UI to Editor pure intents (`InputEvents.h`): the raw input stream, mouse
   move/press/release/scroll plus `ObjectClicked { int objectUid; int modifiers; }`
   and `DeleteRequested { }`. These carry no scene; the UI does not hold or stamp
   the scene (`m_scene` is gone from the panels).
2. UI to Controller editing events (`SceneEvents.h`, `ShaderEvents.h`): complete
   scene mutations (selection, transform, visibility, property edits, shader
   edits) carrying `int objectUid`. Panels emit these with the active object's
   integer UID; `Editor::OnUIEvent` enqueues them unchanged and controllers
   resolve the UID via their `ControllerContext`.
3. Editor to Controller wrapped events: `ObjectSelected` (the Editor subscribes
   to `ObjectClicked` and forwards `ObjectSelected{ objectUid, modifiers }`, and
   the Application emits it for the viewport IDBuffer pick), `ObjectDeleteRequested`
   (the Editor subscribes to `DeleteRequested` and forwards
   `ObjectDeleteRequested{ }`), the membership events (`SceneObjectAddRequested` /
   `SceneObjectDeleteRequested`), and the replay/restore events. `CameraEvents`
   carry `int camId` (resolved by the controller).

## Location

| File | Purpose |
|------|---------|
| `src/editor/events/IEventQueue.h` | The only dispatch surface controllers and scripts see: `Subscribe` and synchronous `Emit` - no deferral |
| `src/editor/events/SceneEvents.h` | Scene-domain events (selection, transform, visibility, camera/mesh/light/env/debug property changes) - ephemeral, carry `int objectUid` |
| `src/editor/events/EditorEvents.h` | Cross-component events only (`RenderResetEvent`, `EnvironmentChanged`, `SceneModified`, `LightGpuChanged`, `LightingRebuild`, `SceneObjectGpuUploadRequested`) - pure data, no Qt |
| `src/editor/events/ScriptEvents.h` | Script run boundaries (`ScriptRunStarted`, `ScriptRunFinished`) that bound one run to one undo step |
| `src/editor/events/CameraEvents.h` | Camera input events (rotate, push, slide, zoom, resize) |
| `src/editor/events/InputEvents.h` | Viewport raw input events (mouse move, press, release, scroll) |
| `src/editor/events/ProjectEvents.h` | Project lifecycle events (new, open, save, saveAs) |
| `src/editor/events/AssetEvents.h` | Asset import events (mesh, camera, light add) |
| `src/editor/events/OperationEvents.h` | Undo/redo request events (`UndoRequested`, `RedoRequested`) |
| `src/editor/events/ConfigEvents.h` | Render config change events |
| `src/editor/events/ShaderEvents.h` | Shader editor events (create, compile, code/struct edit, field add) |
| `src/editor/events/GizmoEvents.h` | Modal transform intents (arm, constrain, confirm, cancel) + the shared `GizmoMode`/`GizmoAxis` enums |
| `src/editor/events/UIEvents.h` | QObject singleton - Qt signal definitions for UI-to-Editor dispatch |
| `src/editor/events/EventBus.h` | The concrete `EventQueue`: `IEventQueue` plus the deferred `Enqueue`/`Process` pair the UI boundary uses (zero Qt dependency) |

## Event structs (pure data)

All event structs live in `src/editor/events/` and carry no Qt headers. They are
plain C++ structs passed by const reference through the system.

### SceneEvents.h

Scene-domain events carry plain integer object UIDs (`int objectUid`, 0 = none),
never raw pointers. `UID` is the common base of `Scene` and `ObjectID`, so the
same integer space covers both scene and objects. The scene itself is not carried
in events: scene-scoped handlers obtain the current `Scene` from their
`ControllerContext`. Controllers resolve `objectUid` to the concrete type
(`Mesh*`, `Light*`, `Camera*`, `Environment*`) via the context (typed scene-pool
lookup) and mutate the object directly.

```cpp
// Selection
struct ObjectSelected {
    int objectUid = 0;          // Selected object UID (0 = background click)
    int modifiers = 0;          // Input::Modifiers bitmask
};

struct ObjectDeselected {
    int objectUid = 0;          // Object UID to deselect
};

// Selection replay (dispatched only by SetSelectionOp on undo/redo): absolute
// selection-set restore by UID. Live selection uses the incremental events above.
struct SelectionChanged {
    std::vector<int> selectedUids;      // Ordered selected object UIDs
    int              activeUid = 0;     // Active object UID (0 = none)
};

// Visibility
struct VisibilityChanged {
    int objectUid = 0;
    bool viewportVisible = true;
    bool renderVisible   = true;
};

// Transform
struct PositionChanged { int objectUid; float posX, posY, posZ; };
struct RotationChanged { int objectUid; float rotX, rotY, rotZ; };
struct ScaleChanged    { int objectUid; float sclX, sclY, sclZ; };

// Camera properties
struct CameraTargetChanged { int objectUid; float targetX, targetY, targetZ; };
struct CameraFovChanged    { int objectUid; float fov; };
// Absolute camera pose (position + target) - replay only, dispatched by
// CameraTransformOp on undo/redo (live navigation carries relative deltas).
struct CameraPoseChanged   { int objectUid; float posX, posY, posZ, tarX, tarY, tarZ; };
// Which SCENE camera the viewport looks through; 0 = none, which hands the view
// back to the viewport's own camera. Scene state, deliberately independent of
// selection. One event for both the PropertyPanel checkbox and SetActiveCameraOp
// replay. The handler activates/deactivates, records the op only when the value
// actually changed, and emits a CameraResizeEvent for the newly activated
// camera so it cannot take the view with a stale aspect.
struct ActiveCameraChanged { int camUid; };

// Mesh properties
struct MeshShadowChanged  { int objectUid; bool enabled; };
struct MeshMaterialChanged { int objectUid; bool enabled; };

// Light properties
struct LightPowerChanged      { int objectUid; float power; };
struct LightColorChanged      { int objectUid; float r, g, b; };
struct LightRadiusChanged     { int objectUid; float radius; };
struct LightShadowChanged     { int objectUid; bool enabled; };
struct LightCutoffChanged     { int objectUid; float cutoff; };
struct LightOuterCutoffChanged { int objectUid; float outerCutoff; };

// Environment properties
struct EnvironmentIntensityChanged { int objectUid; float intensity; };
struct EnvironmentRotationChanged  { int objectUid; float rotation; };

// Debug object properties - ONE set for all three debug types
struct DebugColorChanged   { int objectUid; float r, g, b, a; };  // debug tints are vec4
struct DebugOpacityChanged { int objectUid; float opacity; };
struct DebugXRayChanged    { int objectUid; bool xray; };
struct DebugLineWidthChanged   { int objectUid; float width; };
struct DebugLineStippleChanged { int objectUid; bool stipple; };          // smoothing follows it: solid = AA, dashed = none
struct DebugPointTypeChanged      { int objectUid; int pointType; };      // DebugPoints::PointType value
struct DebugPointScaleChanged     { int objectUid; float scale; };
struct DebugProjectionModeChanged { int objectUid; int projectionMode; }; // 0 = screen, 1 = world
struct DebugPositionsChanged { int objectUid; std::vector<float> xyz; };  // whole list, 3 floats/position

// Scene membership (Add / Delete) - UID-carrying (replay-safe; see below)
struct SceneObjectAddRequested { int objectUid; };                          // forward (Editor import) + replay
struct SceneObjectDeleteRequested { std::vector<int> uids; };               // single BATCHED removal path (gesture + replay)
struct ObjectDeleteRequested { };                                           // Editor-wrapped Delete gesture, FORWARD-ONLY
```

Scene events carry integer UIDs. Scene-domain events follow the system-wide
id-carrying rule: they carry `int objectUid`, never object pointers. The
`ObjectController` resolves a property event's UID through the resource pool
(typed lookup, e.g. `Get<Mesh>`) and mutates the object directly; the
`SceneController` resolves selection and membership against the current Scene.
Neither carries a pointer that could dangle. `Editor::OnUIEvent` just enqueues
them unchanged.

One debug event set, three pools. The debug events above are shared by
`DebugLine`, `DebugPoints` and `DebugMesh` rather than split per type: color,
opacity and x-ray are literally the same property on three unrelated pools, so
the handler resolves the UID against whichever pool holds it (`ObjectController`'s
`ForDebugObject`) instead of the event naming a type. A type-specific knob (line
width, point shape) simply never reaches an object that has no such property; an
unknown UID is ignored and records nothing. Every handler ends in `Mutated()`,
which emits `RenderResetEvent` and so marks `DebugDrawBuilder` dirty: a debug
property edit is only visible after a reflatten.

`DebugPositionsChanged` is absolute: it carries the whole position list, so one
event and one op cover editing a coordinate, adding a row and removing a row
alike (a structural change cannot be expressed per index). It is flattened to xyz
triples because this header stays glm-free; `PropertyPanel` flattens on the way
out and the controller unflattens on the way in, dropping a trailing partial
triple.

Membership events carry UIDs too (no scene payload). Add/Delete is split into
Import (Editor: `Load` the resource into the pool, `OnMeshImport`, `OnCameraAdd`,
`OnLightAdd`, ...) and Add (SceneController: fetch the pooled object by UID and
register it). The membership events carry the object UID because they double as
replay events for the `SceneObjectAddOp` undo operation (see
[Operations](operations.md)): a serialized op replays by UID, and the pool is the
single owner of the object. `SceneObjectAddOp` re-dispatches the originating
add/delete events on replay, so undo/redo runs the same controller handlers as
live edits; the handler's `Submit` is muted by `OperationManager`'s
`Phase::Replaying` guard, so playback does not re-record (the expected "Submit
suppressed during replay" LOG).

`ObjectDeleteRequested` is the Editor-wrapped Delete gesture: the Editor
subscribes to the pure `DeleteRequested` intent (Delete key in
Outliner/Viewport) and forwards `ObjectDeleteRequested{}`, so the UI no longer
stamps the scene. It is forward-only (the recorded composite replays via
`SceneObjectDeleteRequested`, never this gesture event). The `SceneController`
snapshots the selection, deselects, emits one batched
`SceneObjectDeleteRequested` carrying all selected UIDs - the removal runs
synchronously inside that emit - so the batched handler is the single removal path
shared by the gesture and by undo/redo replay (no replay-only handling), and
records one composite
(`CompositeOp[SetSelectionOp(before->none), SceneObjectAddOp(uids,false)]`: a
delete of N objects is one op, not N). The forward add path records
`CompositeOp[SceneObjectAddOp({u},true), SetSelectionOp(before->{u})]`: add and
select collapse to one undo entry.

### EditorEvents.h

Cross-component events only, for things that involve a different component than
the emitter (controller to Editor, Editor to Renderer). Scene-domain events
live in `SceneEvents.h`.

```cpp
// Cross-component events (controllers -> Editor, Editor -> Renderer)
struct SceneModified {};                       // mark project dirty
struct LightGpuChanged { int objectUid; };     // single light SSBO update
struct LightingRebuild {};                     // full light SSBO dict rebuild
struct SceneObjectGpuUploadRequested { int objectUid; };  // on-demand MeshGPU/LightGPU/EnvironmentGPU upload (scene add + shadow toggle)
struct RenderResetEvent {};                    // temporal accumulation reset
```

### CameraEvents.h

```cpp
struct CameraRotateEvent  { int camId; float mouse_delta_x, mouse_delta_y; };
struct CameraZoomEvent    { int camId; float scroll_dir; };
struct CameraPushEvent    { int camId; float mouse_delta_x, mouse_delta_y; };
struct CameraSlideEvent   { int camId; float mouse_delta_x, mouse_delta_y; };
struct CameraSpinEvent    { int camId; float mouse_delta_x, mouse_delta_y; };
struct CameraResizeEvent  { int camId; int width; int height; };

// Drag-gesture boundaries - bracket a continuous orbit/pan/dolly manipulation
// so it collapses to ONE undo entry (controller-owned gesture, see
// Controllers). CameraController captures the "before" pose on begin, mutates
// live during the drag WITHOUT recording, and records one CameraTransformOp on
// end.
struct CameraDragBegin { int camId; };
struct CameraDragEnd   { int camId; };
```

Camera events carry the camera's integer UID (never a raw `Camera*`, since the
camera could be deleted mid-drag; a UID stays stable). `CameraController` resolves
`camId` through the resource pool, not the scene's `cam_list`, so the pooled
viewport camera (which is never scene content) is navigated by the same events.
Scroll zoom has no press/release, so it is not bracketed: `CameraZoomEvent`
records per-event and relies on `CameraZoomOp::MergeKey()` to coalesce a scroll
burst into one undo step.

### InputEvents.h

```cpp
struct MouseMoveEvent   { glm::vec2 position, delta; Modifiers mods; bool left/middle/rightHeld; };
struct MousePressEvent  { MouseButton button; glm::vec2 position; Modifiers mods; };
struct MouseReleaseEvent{ MouseButton button; glm::vec2 position; Modifiers mods; };
struct MouseScrollEvent { float delta; glm::vec2 position; Modifiers mods; bool ...Held; };

// Keyboard. `key` is an Input::Key, never a raw Qt code, so an unmapped key arrives
// as Key_Unknown and no handler can act on a stray number. Auto-repeat is filtered at
// the Qt boundary (Viewport::keyPressEvent): a modal operator reacts to a press, not
// to the OS repeat rate. There is deliberately no KeyReleaseEvent - nothing is chorded
// on a held key, and the modal operators are press-to-enter / press-to-leave.
struct KeyPressEvent    { Input::Key key; Modifiers mods; };

// A raw viewport fact, not a transform intent, which is why it lives here and not in
// GizmoEvents.h: any controller holding a modal state machine must abandon it when the
// user clicks into another panel.
struct ViewportFocusLost { };

// Pure UI->Editor intents (no scene pointer - the Editor wraps them):
struct ObjectClicked    { int objectUid; int modifiers; };  // Outliner row click
struct DeleteRequested  { };                                 // Delete key (Outliner/Viewport)
```

### ConfigEvents.h

```cpp
// Whole-config change (UI panel emits it; SetRenderConfigOp replays it).
struct RenderConfigChangedEvent { RenderConfig config; };

// Slider-drag boundaries - same controller-owned gesture pattern as the camera
// drag (see Controllers). RenderConfigController captures the "before" config on
// begin, applies intermediate values live WITHOUT recording, and records one
// SetRenderConfigOp on end. Discrete edits (checkbox, combo box) arrive with no
// gesture and are recorded immediately, one op each.
struct ConfigEditBegin {};
struct ConfigEditEnd   {};
```

### ShaderEvents.h

Events for the shader editor pipeline (UI to Editor to ShaderController). All
events carry an `int objectUid` (the target mesh's UID, resolved once by the
`ShaderEditorPanel` from the active scene selection). `ShaderSection` identifies
which `ShaderStruct` container is edited.

```cpp
enum class ShaderSection : int {
    Attributes = 0, PassOutputs = 1, Inputs = 2, Outputs = 3,
    Uniforms = 4, StructDefs = 5, Functions = 6, PushConstants = 7
};

struct ShaderCreateRequested  { int objectUid; };
struct ShaderCompileRequested { int objectUid; int stage; int unitType; };  // 0=Code path, 1=Struct path
struct ShaderCodeEdited       { int objectUid; int stage; std::string code; };
struct ShaderStructEdited     { int objectUid; int stage; ShaderSection section;
                                int fieldIndex; int subFieldIndex; std::string field; std::string value; };
struct ShaderFieldAdded       { int objectUid; int stage; ShaderSection section; int subFieldIndex; };

// Undo/redo wiring (content edits only - see Operations)
struct ShaderEditBegin        { int objectUid; int stage; };  // gesture start: snapshot "before" code
struct ShaderEditEnd          { int objectUid; int stage; };  // gesture end: record one SetShaderCodeOp if changed
// Replayed restore events (mirror the forward edit events 1:1, but bump version)
struct ShaderCodeRestored     { int objectUid; int stage; std::string code; };
struct ShaderFieldRestored    { int objectUid; int stage; ShaderSection section;
                                int fieldIndex; ShaderFieldValue value; };  // whole element to assign back
struct ShaderFieldAddRestored { int objectUid; int stage; ShaderSection section; int subFieldIndex; };  // redo of an add
struct ShaderFieldRemoved     { int objectUid; int stage; ShaderSection section; int subFieldIndex; };  // undo of an add
// Undo/redo of Create Shader (replayed by ShaderLinkOp; Editor-handled)
struct ShaderLinkRestored   { int objectUid; int shaderId; };  // redo: relink pooled shader
struct ShaderUnlinkRestored { int objectUid; };                // undo: drop the reference
```

Version flow: only `ShaderCreateRequested` and `ShaderCompileRequested` bump
`Shader::m_version` (and only on successful compile). The other events mutate
data without recompiling; the user must press Compile to apply changes to the GPU
pipeline. Because create/compile rebuild the pipeline, `ShaderController`
emits `RenderResetEvent` after both, so temporal accumulation resets.

Undo/redo flow: content edits are recorded as delta-only ops matching each edit
event: `SetShaderCodeOp` (before/after code), `SetShaderFieldOp` (before/after of
one whole IR element, a `ShaderFieldValue` variant over
`S_IO`/`S_Uniform`/`S_Func`/`S_PushConstant`/`S_StructDef`) and
`AddShaderFieldOp` (append vs remove one default entry). The forward
`ShaderStructEdited` still delivers a single `{subFieldIndex, field, value}`; the
controller applies it onto the live element and snapshots the element before/after
for the op. Code edits are bracketed by `ShaderEditBegin`/`ShaderEditEnd` so a
keystroke burst collapses to one undo entry on focus-out; discrete struct/field
edits record one op each. Undo/redo replays a dedicated restore event
(`ShaderCodeRestored`/`ShaderFieldRestored`/`ShaderFieldAddRestored`/
`ShaderFieldRemoved`) that re-applies one edit dimension and bumps
`ShaderUnit::m_version` (panel refresh), CPU-only, no recompile to SPIR-V. Create
Shader is undoable via `ShaderLinkOp` (pool-preserving membership toggle: undo
drops the reference, redo relinks the pooled shader by UID, via
`ShaderLinkRestored`/`ShaderUnlinkRestored`); Compile stays non-undoable. See
[Operations](operations.md).

### GizmoEvents.h

Intents for the modal transform gesture (Editor keystroke to
`TransformGizmoController`). `GizmoMode` and `GizmoAxis` live here rather than in
`TransformGizmo.h`: an event header must not depend on the controller's types, and
both enums are shared vocabulary between the events, the state machine and the
draw builder.

```cpp
enum class GizmoMode { None = 0, Move, Rotate, Scale };   // G / R / S
enum class GizmoAxis { None = 0, X, Y, Z };               // X / Y / Z, None = screen space

struct GizmoModeRequested { int objectUid; GizmoMode mode; };  // only this one carries a uid
struct GizmoAxisRequested { GizmoAxis axis; };
struct GizmoConfirmed     { };   // LMB or Return
struct GizmoCancelled     { };   // Esc, RMB, or viewport focus loss
```

`GizmoAxis::None` is a live state, not an absent one. It means the view plane for
Move, uniform for Scale and the view axis for Rotate, both the state a bare
G/R/S leaves behind and the state `W` returns to. So `W` emits
`GizmoAxisRequested{GizmoAxis::None}`; there is no separate "unconstrain" event.

Every branch of the Editor's key handler emits unconditionally. Nothing in the
Editor reads gizmo state to decide whether to emit, and the controller drops what
it cannot use. This is not defensive coding: both key events arrive through the
deferred UI boundary, `Process()` pops them one at a time in FIFO order, and a
mode handler's synchronous work is complete before the axis event is popped, so a
gate in the Editor would silently swallow an axis key that arrived in the same
frame as its mode key.

Cursor motion is not a gizmo event. The controller subscribes to the existing
`MouseMoveEvent` and ignores it while nothing is armed, reading `e.position`
rather than `EditorViewport::Cursor()`: the Editor's own subscription is what
pushes the cursor into the viewport, and dispatch is registration-ordered, so
reading it back would consume a one-frame-stale cursor if the two subscriptions
were ever registered the other way round.

Only the gesture-opening event carries a uid, following `CameraEvents.h`. The
later events need none: the controller already holds the target it latched when
the gesture began, which is also why changing the Outliner selection mid-gesture
does not move the gesture. See [Transform gizmo](gizmo.md).

## UIEvents (Qt signal bus)

A QObject singleton providing typed Qt signals. Panels connect to these signals
directly; the Editor never touches them, the bridge is in Application.

```cpp
class UIEvents : public QObject {
    Q_OBJECT
public:
    static UIEvents& instance();
};

// Profiling is always collected while the app runs: DeferredRenderer populates
// a FrameProfile every frame (no toggle) and DrawFrame() returns it. Application
// assembles the UIContext each frame (Editor never produces it), setting
// UIContext::profile to the returned FrameProfile (no UIEvents signal, no Editor
// copy).
//
// Panel signals (defined on the panel itself, not UIEvents):
//   Outliner::objectClicked(const ObjectClicked& e)
//   Outliner::visibilityChanged(const VisibilityChanged& e)
//   RenderConfigPanel::configValueChanged(const RenderConfigChangedEvent& e)
//   Viewport::mouseMoved(const MouseMoveEvent& e)
//   ...
```

## EventQueue (typed dispatcher, no Qt)

Header-only, zero Qt dependency, Editor-internal: `src/render/` never includes it,
and the Renderer is reached through `RenderContext` and the Editor's upload calls,
never through an event. Two surfaces share one implementation:

- `IEventQueue` (`src/editor/events/IEventQueue.h`) is what controllers and
  scripts are given: `Subscribe<TEvent>(handler)` and `Emit<TEvent>(event)`. It
  has no way to defer, so anything a controller or a script emits has been applied
  before the emitting call returns.
- `EventQueue` (`src/editor/events/EventBus.h`) adds `Enqueue<TEvent>(event)`,
  `Process(int maxEvents = 1000)` and `PendingCount()`. `Enqueue` is a property of
  the concrete queue, not of the interface, because deferral exists for exactly
  one caller: `Editor::OnUIEvent`.

```cpp
// Subscribe to typed events
eventBus.Subscribe<LightGpuChanged>([](const LightGpuChanged& e) {
    uploadSingleLight(/* resolve e.objectUid via the pool */);
});

// Emit synchronously: every handler has run on return
ctx.events.Emit(PositionChanged{uid, x, y, z});

// Defer to the next Process() - the UI boundary only
eventBus.Enqueue(ObjectClicked{uid, modifiers});

// Drain the deferred queue (Editor::Edit(), once per frame)
eventBus.Process();
```

Properties:

- Header-only, zero Qt dependency; safe to include from any layer, but only the
  Editor does
- `Emit` dispatches synchronously and depth-first: a handler's own emits complete
  before it returns, so the caller sees the whole consequence chain
- `Enqueue`/`Process` are a deferred FIFO queue, drained once per frame by
  `Editor::Edit()`; `Process` also drains anything a handler enqueued while it
  ran, up to the `maxEvents` cap
- Synchronous dispatch depth is capped at `EventQueue::kMaxDispatchDepth` (64).
  A handler cycle recurses instead of queueing, so at the limit `Dispatch` logs
  `NEURUS_ERR` naming the event type and drops the event, without invoking
  handlers. `Process()` saves the entry depth, zeroes it for the drain and
  restores it afterwards, so one runaway chain cannot poison later frames
- Not thread-safe; all calls on the main thread

## Template-based event forwarding

The bridge from Qt signals to the Editor's internal `EventQueue` uses two
template helpers in `Application`.

### Step 1 - Panel defines a Qt signal

```cpp
// Outliner.h
signals:
    void objectClicked(const ObjectClicked& e);
    void visibilityChanged(const VisibilityChanged& e);
    void deleteRequested(const DeleteRequested& e);
```

### Step 2 - Application wires via ConnectUIEvent

```cpp
// Application.h
template<typename Panel, typename Event>
void ConnectUIEvent(QObject* sender, void (Panel::*signal)(const Event&))
{
    QObject::connect(
        static_cast<Panel*>(sender), signal,
        [editor = app_editor.get()](const Event& e) {
            editor->OnUIEvent(e);
        });
}

// Application.cpp - one line per panel signal
ConnectUIEvent(outliner, &Outliner::objectClicked);
ConnectUIEvent(outliner, &Outliner::visibilityChanged);
ConnectUIEvent(outliner, &Outliner::deleteRequested);
ConnectUIEvent(viewport, &Viewport::mouseMoved);
ConnectUIEvent(viewport, &Viewport::mouseScrolled);
```

### Step 3 - Editor::OnUIEvent enqueues

```cpp
// Editor.h - the only deferred call site in production
template<typename Event>
void OnUIEvent(const Event& e) {
    ed_eventBus.Enqueue<Event>(e);
}
```

### Step 4 - Controller subscribes

```cpp
// SceneController::Init() - selection and membership via free-function handlers
ctx.events.Subscribe<ObjectSelected>([ctx](const ObjectSelected& e) {
    OnObjectSelected(e, ctx);
});
ctx.events.Subscribe<SceneObjectDeleteRequested>([ctx](const SceneObjectDeleteRequested& e) {
    OnSceneObjectDeleteRequested(e, ctx);
});

// ObjectController::Init() - property writes, resolved through the pool
ctx.events.Subscribe<PositionChanged>([ctx](const PositionChanged& e) {
    OnPositionChanged(e, ctx);
});

// Editor::Initialize() - handles cross-component GPU-sync events
ed_eventBus.Subscribe<LightGpuChanged>([this](const LightGpuChanged& e) {
    auto light = m_resources->Get<Light>(e.objectUid);
    if (!light) return;
    auto gpuStruct = ed_uploadManager->UploadLighting(*light);
    ed_renderer->GetRenderCache().UpdateLight(e.objectUid, gpuStruct);
});
ed_eventBus.Subscribe<LightingRebuild>([this](const LightingRebuild&) {
    UploadLighting();
});
ed_eventBus.Subscribe<SceneModified>([this](const SceneModified&) {
    m_dirty = true;
});
```

## Data flow summary

```
User Input (QML / Qt widgets)
    |
    v
UI Panel (Qt signal, e.g. Outliner::objectClicked - pure intent)
    |
    v
Application::ConnectUIEvent<T>   <- single wiring point
    |  [lambda calls editor->OnUIEvent(e)]
    v
Editor::OnUIEvent<T>             <- generic template, DEFERRED here
    |  eventBus.Enqueue<T>(e)
    v
EventQueue::Process()            <- in Edit(), once per frame
    |
    v
SceneController / ObjectController / Editor handlers   <- business logic
    |- Select / Deselect, add / delete (SceneController)
    |- property writes, resolved through the pool (ObjectController)
    |- GPU uploads, render config, project state (Editor)
```

Everything below `OnUIEvent` is synchronous: a handler that emits another event
runs that event's handlers before it returns, so the diagram is a call tree, not
a second queue.

## Adding a new event

1. Define the event struct in the appropriate `src/editor/events/*.h` file (pure
   data, no Qt includes).
2. Emit it from the panel as a Qt signal.
3. Wire it in Application with one `ConnectUIEvent` call.
4. Subscribe in Editor with one `eventBus.Subscribe<T>()` call.

That is four changes, all in different layers, no cross-layer includes needed.

## RenderResetEvent

```cpp
struct RenderResetEvent
{
};
```

An empty marker event emitted whenever the scene state changes in a way that
invalidates temporal accumulation (camera movement, light/object transform,
visibility toggle, config change, project load, asset import, environment change,
shader create/compile). Camera movement events are emitted by `CameraController`
after each frame handler (Zoom, Rotate, Push, Slide, Resize). Shader create and
compile events are emitted by `ShaderController` after the shader version bumps.
Scene property writes are handled by `ObjectController`, scene membership by
`SceneController`; both emit `RenderResetEvent{}` through the shared
`neurus::notify` helpers after every mutation.

Subscribers listen for this event to reset per-frame history:

- `DeferredRenderer::ResetShadowAccumulation()` zeros the shadow accumulation
  iteration counter.
- Future: SSAO temporal accumulation, SSR temporal accumulation, TAA jitter reset.

## Controller GPU-sync flow

Scene mutations that affect GPU resources follow a three-tier event pattern:

```
SceneController / ObjectController mutation handler
  |- Resolves the object UID - against the pool (ObjectController) or the
  |   current Scene (SceneController) - and mutates the object
  |- Emits one or more EditorEvents, synchronously:
        |- SceneModified{} -> Editor marks project dirty
        |- RenderResetEvent{} -> DeferredRenderer resets temporal accumulation
        |
        |- Scene object enters scene (add / undo-redo re-add) or light
        |   shadow toggled ON -> SceneObjectGpuUploadRequested{objectUid}
        |     |- Editor::OnSceneObjectGpuUpload handler:
        |         resolves the pooled object by UID and uploads on demand
        |         (MeshGPU / LightGPU shadow maps / IBL), skip-if-cached
        |
        |- Light property change -> LightGpuChanged{objectUid}
        |     |- Editor::LightGpuChanged handler:
        |         resolves the pooled Light*, calls UploadLighting(*light),
        |         calls RenderCache::UpdateLight(id, gpuStruct)
        |         -> single SSBO struct updated
        |
        |- Light transform/visibility/shadow toggle -> LightingRebuild{}
              |- Editor::LightingRebuild handler:
                  calls UploadLighting() -> full light SSBO dict rebuilt
                  -> all light SSBOs re-uploaded
```

The key invariant: controllers never touch GPU resources. They mutate scene
objects and emit `EditorEvents`. The Editor handles all GPU uploads in its event
handlers, and because dispatch is synchronous the upload has already run when the
mutation handler returns. This keeps the controllers stateless and preserves the
Editor-to-Renderer GPU upload boundary.
