# Event System

## Overview

Neurus uses a **two-tier event system** to enforce layer isolation. UI panels
emit Qt signals; a template bridge in Application converts these to typed
event structs; Editor subscribes and handles them through an internal
EventQueue. The result: panels never include Editor headers, Editor has zero
Qt dependencies, and all cross-layer wiring is auditable in one file.

```
Panel Qt signal (e.g. Outliner::objectClicked - pure input intent)
  └── Application::ConnectUIEvent<Outliner, ObjectClicked>
        └── [lambda calls Editor::OnUIEvent(e)]
              └── EventQueue::enqueue<ObjectClicked>
                    └── EventQueue::Process() → Editor wraps to ObjectSelected, then subscriber handles it
```

**ID-carrying events - the current event model**

Every event in Neurus is ephemeral: enqueued and processed within a single
frame, then destroyed once `EventQueue::Process()` returns. Events carry plain
**integer object UIDs** (never raw `UID*`/`Camera*` pointers) — a raw pointer
can dangle after the object is deleted, while a UID is a stable identity that
controllers resolve against the current scene/pool at dispatch time. `UID` is
the common base of `Scene` and `ObjectID`, so the same integer space covers
both. Controllers resolve an event's `int objectUid` to the live object via
their `ControllerContext` (scene lookup for property edits, pool lookup for
membership) — see `editor.instructions.md`. The scene itself is NOT carried in
events: scene-scoped handlers obtain the current Scene from the context, so a
scene swap on New/Load can never leave an event holding a stale pointer. This
id-based model is what makes pointer-free event payloads safe across frames,
serialization, and undo/redo replay.

### Three event paths

Cross-layer traffic flows along three distinct paths, all still "UI emits,
Editor wraps or forwards, controllers handle":

1. **UI -> Editor pure intents** (`InputEvents.h`): the raw input stream -
   mouse move/press/release/scroll plus `ObjectClicked { int objectUid; int modifiers; }`
   and `DeleteRequested { }`. These carry no scene; the UI does not
   hold or stamp the scene (`m_scene` is gone from the panels).
2. **UI -> Controller editing events** (`SceneEvents.h`, `ShaderEvents.h`):
   complete scene mutations (selection, transform, visibility, property edits,
   shader edits) carrying `int objectUid`. Panels emit these with the active
   object's integer UID; `Editor::OnUIEvent` enqueues them unchanged and
   controllers resolve the UID against the current scene via their
   `ControllerContext`.
3. **Editor -> Controller wrapped events**: `ObjectSelected` (the
   Editor subscribes to `ObjectClicked` and forwards
   `ObjectSelected{ objectUid, modifiers }`, and the Application
   emits it for the viewport IDBuffer pick), `ObjectDeleteRequested` (the
   Editor subscribes to `DeleteRequested` and forwards
   `ObjectDeleteRequested{ }`), the membership events
   (`SceneObjectAddRequested` / `SceneObjectDeleteRequested`), and the replay /
   restore events. `CameraEvents` carry `int camId` (resolved by the controller).

## Location

| File | Purpose |
|------|---------|
| `src/editor/events/SceneEvents.h` | Scene-domain events (selection, transform, visibility, camera/mesh/light/env property changes) - ephemeral, carry `int objectUid` |
| `src/editor/events/EditorEvents.h` | Cross-component events only (RenderResetEvent, EnvironmentChanged, SceneModified, LightGpuChanged, LightingRebuild, SceneObjectGpuUploadRequested) — pure data, no Qt |
| `src/editor/events/CameraEvents.h` | Camera input events (rotate, push, slide, zoom, resize) |
| `src/editor/events/InputEvents.h` | Viewport raw input events (mouse move, press, release, scroll) |
| `src/editor/events/ProjectEvents.h` | Project lifecycle events (new, open, save, saveAs) |
| `src/editor/events/AssetEvents.h` | Asset import events (mesh, camera, light add) |
| `src/editor/events/OperationEvents.h` | Undo/redo request events (`UndoRequested`, `RedoRequested`) |
| `src/editor/events/ConfigEvents.h` | Render config change events |
| `src/editor/events/ShaderEvents.h` | Shader editor events (create, compile, code/struct edit, field add) |
| `src/editor/events/UIEvents.h` | QObject singleton — Qt signal definitions for UI→Editor dispatch |
| `src/editor/events/EventBus.h` | Header-only typed event dispatcher (`EventQueue` class, zero Qt dependency) |

## Event Structs (Pure Data)

All event structs live in `src/editor/events/` and carry no Qt headers.
They are plain C++ structs passed by const reference through the system.

### SceneEvents.h

Scene-domain events carry plain **integer object UIDs** (`int objectUid`,
0 = none), never raw pointers. `UID` is the common base of `Scene` and
`ObjectID`, so the same integer space covers both scene and objects. The
scene itself is NOT carried in events — scene-scoped handlers obtain the
current `Scene` from their `ControllerContext`. Controllers resolve
`objectUid` to the concrete type (Mesh*, Light*, Camera*, Environment*) via
the context (typed scene-pool lookup) and mutate the object directly.

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
// Absolute camera pose (position + target) — replay only, dispatched by
// CameraTransformOp on undo/redo (live navigation carries relative deltas).
struct CameraPoseChanged   { int objectUid; float posX, posY, posZ, tarX, tarY, tarZ; };

// Mesh properties
struct MeshShadowChanged  { int objectUid; bool enabled; };
struct MeshMaterialChanged { int objectUid; bool enabled; };

// Light properties
struct LightPowerChanged      { int objectUid; float power; };
struct LightRadiusChanged     { int objectUid; float radius; };
struct LightShadowChanged     { int objectUid; bool enabled; };
struct LightCutoffChanged     { int objectUid; float cutoff; };
struct LightOuterCutoffChanged { int objectUid; float outerCutoff; };

// Environment properties
struct EnvironmentIntensityChanged { int objectUid; float intensity; };
struct EnvironmentRotationChanged  { int objectUid; float rotation; };

// Debug object properties (issue #22) — ONE set for all three debug types
struct DebugColorChanged   { int objectUid; float r, g, b, a; };  // debug tints are vec4
struct DebugOpacityChanged { int objectUid; float opacity; };
struct DebugXRayChanged    { int objectUid; bool xray; };
struct DebugLineWidthChanged   { int objectUid; float width; };
struct DebugLineStippleChanged { int objectUid; bool stipple; };
struct DebugLineSmoothChanged  { int objectUid; bool smooth; };
struct DebugPointTypeChanged      { int objectUid; int pointType; };      // DebugPoints::PointType value
struct DebugPointScaleChanged     { int objectUid; float scale; };
struct DebugProjectionModeChanged { int objectUid; int projectionMode; }; // 0 = screen, 1 = world
struct DebugPositionsChanged { int objectUid; std::vector<float> xyz; };  // whole list, 3 floats/position

// Scene membership (Add / Delete) — UID-carrying (replay-safe; see below)
struct SceneObjectAddRequested { int objectUid; };                          // forward (Editor import) + replay
struct SceneObjectDeleteRequested { std::vector<int> uids; };               // single BATCHED removal path (gesture + replay)
struct ObjectDeleteRequested { };                                           // Editor-wrapped Delete gesture, FORWARD-ONLY
```

**Scene Events Carry Integer UIDs**

Scene-domain events follow the system-wide id-carrying rule (see Overview
above): they carry `int objectUid`, never object pointers. Controllers resolve
the UID against the current Scene (typed pool lookup, e.g. `mesh_list.find`)
via their `ControllerContext` and mutate the object directly — no pointer
payload to dangle. `Editor::OnUIEvent` just enqueues them unchanged.

**One debug event set, three pools.** The debug events above are shared by
`DebugLine`, `DebugPoints` and `DebugMesh` rather than split per type: color,
opacity and x-ray are literally the same property on three unrelated pools, so
the handler resolves the UID against whichever pool holds it
(`SceneController`'s `ForDebugObject`) instead of the event naming a type. A
type-specific knob (line width, point shape) simply never reaches an object that
has no such property — an unknown UID is ignored and records nothing. Every
handler ends in `Mutated()`, which enqueues `RenderResetEvent` and so marks
`DebugDrawBuilder` dirty: a debug property edit is only visible after a
reflatten.

`DebugPositionsChanged` is **absolute** — it carries the whole position list, so
one event and one op cover editing a coordinate, adding a row and removing a row
alike (a structural change cannot be expressed per index). It is flattened to
xyz triples because this header stays glm-free; `PropertyPanel` flattens on the
way out and the controller unflattens on the way in, dropping a trailing partial
triple.

**Membership events carry UIDs too (no scene payload).** Add/Delete is split
into **Import** (Editor: `Load` the resource into the pool — `OnMeshImport`,
`OnCameraAdd`, `OnLightAdd`, ...) and **Add** (SceneController: fetch the
pooled object by UID and register it). The membership events carry the object
UID because they double as replay events for the `SceneObjectAddOp` undo
operation (see operation-system.instructions.md): a serialized op replays by
UID, and the pool is the single owner of the object.
`SceneObjectAddOp` re-dispatches the originating add/delete events on replay,
so undo/redo runs the same controller handlers as live edits; the handler's
`Submit` is muted by `OperationManager`'s `Phase::Replaying` guard, so
playback does not re-record (the expected "Submit suppressed during replay"
LOG).
`ObjectDeleteRequested` is the Editor-wrapped Delete gesture: the Editor
subscribes to the pure `DeleteRequested` intent (Delete key in
Outliner/Viewport) and forwards `ObjectDeleteRequested{}` — the UI no longer
stamps the scene. It is **forward-only** (the recorded composite replays via
`SceneObjectDeleteRequested`, never this gesture event). The SceneController
snapshots the selection, guards the last camera, deselects, and DEFERS the
actual removals as ONE batched `SceneObjectDeleteRequested` carrying all
selected UIDs — so the batched handler is the SINGLE removal path shared by
the gesture and by undo/redo replay (no replay-only handling), and records ONE
light composite (`CompositeOp[SetSelectionOp(before→∅), SceneObjectAddOp(uids,false)]` — a
delete of N objects is ONE op, not N). The forward add path records
`CompositeOp[SceneObjectAddOp({u},true), SetSelectionOp(before→{u})]` — add
AND select collapse to one undo entry.

### EditorEvents.h

Cross-component events only — things that involve a DIFFERENT component than
the emitter (SceneController -> Editor, Editor -> Renderer). Scene-domain events
live in SceneEvents.h.

```cpp
// Cross-component events (SceneController -> Editor, Editor -> Renderer)
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

// Drag-gesture boundaries — bracket a continuous orbit/pan/dolly manipulation
// so it collapses to ONE undo entry (controller-owned gesture, see
// editor.instructions.md → Undo/Redo). CameraController captures the "before"
// pose on begin, mutates live during the drag WITHOUT recording, and records
// one CameraTransformOp on end.
struct CameraDragBegin { int camId; };
struct CameraDragEnd   { int camId; };
```

Camera events carry the camera's integer UID (never a raw `Camera*` — the
camera could be deleted mid-drag; a UID stays stable). CameraController resolves
`camId` against the current scene's `cam_list` via its `ControllerContext`.
Scroll zoom has no press/release, so it is NOT bracketed: `CameraZoomEvent`
records per-event and relies on `CameraZoomOp::MergeKey()` to coalesce a
scroll burst into one undo step.

### InputEvents.h

```cpp
struct MouseMoveEvent   { int x, y; float dx, dy; Modifiers mods; };
struct MousePressEvent  { int x, y; MouseButton btn; Modifiers mods; };
struct MouseReleaseEvent{ int x, y; MouseButton btn; Modifiers mods; };
struct MouseScrollEvent { int x, y; float delta; Modifiers mods; };

// Pure UI->Editor intents (no scene pointer - the Editor wraps them):
struct ObjectClicked    { int objectUid; int modifiers; };  // Outliner row click
struct DeleteRequested  { };                                 // Delete key (Outliner/Viewport)
```

### ConfigEvents.h

```cpp
// Whole-config change (UI panel emits it; SetRenderConfigOp replays it).
struct RenderConfigChangedEvent { RenderConfig config; };

// Slider-drag boundaries — same controller-owned gesture pattern as the camera
// drag (see editor.instructions.md → Undo/Redo). RenderConfigController captures
// the "before" config on begin, applies intermediate values live WITHOUT
// recording, and records one SetRenderConfigOp on end. Discrete edits (checkbox,
// combo box) arrive with no gesture and are recorded immediately, one op each.
struct ConfigEditBegin {};
struct ConfigEditEnd   {};
```

### ShaderEvents.h

Events for the shader editor pipeline (UI → Editor → ShaderController). All events
carry an `int objectUid` (the target mesh's UID, resolved once by the
ShaderEditorPanel from the active scene selection). `ShaderSection` identifies
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

// Undo/redo wiring (content edits only — see operation-system.instructions.md)
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

**Version flow:** Only `ShaderCreateRequested` and `ShaderCompileRequested` bump
`Shader::m_version` (and only on successful compile). The other events mutate
data without recompiling — the user must press Compile to apply changes to the
GPU pipeline. Because create/compile rebuild the pipeline, `ShaderController`
enqueues `RenderResetEvent` after both, so temporal accumulation resets.

**Undo/redo flow:** Content edits are recorded as delta-only ops matching each
edit event: `SetShaderCodeOp` (before/after code), `SetShaderFieldOp`
(before/after of one whole IR element — a `ShaderFieldValue` variant over
`S_IO`/`S_Uniform`/`S_Func`/`S_PushConstant`/`S_StructDef`) and `AddShaderFieldOp`
(append vs remove one default entry). The forward `ShaderStructEdited` still
delivers a single `{subFieldIndex, field, value}`; the controller applies it onto
the live element and snapshots the element before/after for the op. Code edits are
bracketed by `ShaderEditBegin`/`ShaderEditEnd` so a keystroke burst collapses to
one undo entry on focus-out; discrete struct/field edits record one op each.
Undo/redo replays a dedicated restore event
(`ShaderCodeRestored` / `ShaderFieldRestored` / `ShaderFieldAddRestored` /
`ShaderFieldRemoved`) that re-applies one edit dimension and bumps
`ShaderUnit::m_version` (panel refresh) — CPU-only, no recompile to SPIR-V.
Create Shader IS undoable via `ShaderLinkOp` (pool-preserving membership
toggle - undo drops the reference, redo relinks the pooled shader by UID, via
`ShaderLinkRestored` / `ShaderUnlinkRestored`); Compile stays non-undoable. See
[operation-system.instructions.md](operation-system.instructions.md).

## UIEvents (Qt Signal Bus)

A QObject singleton providing typed Qt signals. Panels connect to these
signals directly; Editor never touches them — the bridge is in Application.

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

## EventQueue (Typed Dispatcher, No Qt)

Header-only template-based dispatcher for Editor-internal event routing.
Events are enqueued and batch-processed via `Process()`.

```cpp
// Subscribe to typed events
eventBus.subscribe<LightGpuChanged>([](const LightGpuChanged& e) {
    uploadSingleLight(/* resolve e.objectUid via the pool */);
});

// Enqueue events (deferred dispatch)
eventBus.enqueue(LightGpuChanged{lightUid});

// Process all queued events (call once per frame or on input)
eventBus.Process();
```

**Properties:**
- Header-only, zero Qt dependency — safe to include from any layer
- Deferred FIFO queue: `enqueue()` stores, `Process()` dispatches
- Re-entrant safe: events emitted from handlers are appended to the queue
- Max events cap (default 1000) prevents infinite loops
- Not thread-safe — all calls on main thread

## Template-Based Event Forwarding

The bridge from Qt signals to the Editor's internal EventQueue uses two
template helpers in `Application`:

### Step 1 — Panel defines a Qt signal

```cpp
// Outliner.h
signals:
    void objectClicked(const ObjectClicked& e);
    void visibilityChanged(const VisibilityChanged& e);
    void deleteRequested(const DeleteRequested& e);
```

### Step 2 — Application wires via ConnectUIEvent

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

// Application.cpp — one line per panel signal
ConnectUIEvent(outliner, &Outliner::objectClicked);
ConnectUIEvent(outliner, &Outliner::visibilityChanged);
ConnectUIEvent(outliner, &Outliner::deleteRequested);
ConnectUIEvent(viewport, &Viewport::mouseMoved);
ConnectUIEvent(viewport, &Viewport::mouseScrolled);
```

### Step 3 — Editor::OnUIEvent enqueues

```cpp
// Editor.h
template<typename Event>
void OnUIEvent(const Event& e) {
    ed_eventBus.enqueue<Event>(e);
}
```

### Step 4 — Controller subscribes

```cpp
// SceneController::Init() — handles scene-domain events via free-function handlers
ctx.events.subscribe<ObjectSelected>([ctx](const ObjectSelected& e) {
    OnObjectSelected(e, ctx);
});
ctx.events.subscribe<VisibilityChanged>([ctx](const VisibilityChanged& e) {
    OnVisibilityChanged(e, ctx);
});

// Editor::Initialize() — handles cross-component GPU-sync events
ed_eventBus.subscribe<LightGpuChanged>([this](const LightGpuChanged& e) {
    auto light = m_resources->Get<Light>(e.objectUid);
    if (!light) return;
    auto gpuStruct = ed_uploadManager->UploadLighting(*light);
    ed_renderer->GetRenderCache().UpdateLight(e.objectUid, gpuStruct);
});
ed_eventBus.subscribe<LightingRebuild>([this](const LightingRebuild&) {
    UploadLighting();
});
ed_eventBus.subscribe<SceneModified>([this](const SceneModified&) {
    m_dirty = true;
});
```

## Data Flow Summary

```
User Input (QML / Qt widgets)
    │
    ▼
UI Panel (Qt signal, e.g. Outliner::objectClicked - pure intent)
    │
    ▼
Application::ConnectUIEvent<T>   ← single wiring point
    │  [lambda calls editor->OnUIEvent(e)]
    ▼
Editor::OnUIEvent<T>             ← generic template
    │  eventBus.enqueue<T>(e)
    ▼
EventQueue<T>::Process()         ← batch dispatch
    │
    ▼
SceneController / Editor handlers ← business logic
    ├── Select / Deselect (SceneController)
    ├── SetVisible / transform setters (SceneController)
    └── SetRenderConfig / GPU uploads (Editor)
```

## Adding a New Event

1. **Define the event struct** in the appropriate `src/editor/events/*.h` file
   (pure data, no Qt includes).
2. **Emit it from the panel** as a Qt signal.
3. **Wire it in Application** with one `ConnectUIEvent` call.
4. **Subscribe in Editor** with one `eventBus.subscribe<T>()` call.

That's it — four changes, all in different layers, no cross-layer includes needed.

### RenderResetEvent

```cpp
struct RenderResetEvent
{
};
```

An empty marker event emitted whenever the scene state changes in a way that
invalidates temporal accumulation (camera movement, light/object transform,
visibility toggle, config change, project load, asset import, environment change,
shader create/compile). Camera movement events are enqueued by `CameraController`
after each frame handler (Zoom, Rotate, Push, Slide, Resize). Shader create and
compile events are enqueued by `ShaderController` after the shader version bumps.
Scene mutations are handled by `SceneController`, which enqueues
`RenderResetEvent{}` via the shared `Mutated()` helper after every mutation.

**Subscribers** listen for this event to reset per-frame history:
- `DeferredRenderer::ResetShadowAccumulation()` — zeros the shadow accumulation iteration counter.
- Future: SSAO temporal accumulation, SSR temporal accumulation, TAA jitter reset.

### SceneController GPU-Sync Flow

Scene mutations that affect GPU resources follow a three-tier event pattern:

```
SceneController mutation handler
  ├── Resolves the object UID from the event's int objectUid against the
  │   current Scene (via the ControllerContext) and mutates the object
  └── Enqueues one or more EditorEvents:
        ├── SceneModified{} → Editor marks project dirty
        ├── RenderResetEvent{} → DeferredRenderer resets temporal accumulation
        │
        ├── Scene object enters scene (add / undo-redo re-add) or light
        │   shadow toggled ON → SceneObjectGpuUploadRequested{objectUid}
        │     └── Editor::OnSceneObjectGpuUpload handler:
        │         resolves the pooled object by UID and uploads on demand
        │         (MeshGPU / LightGPU shadow maps / IBL), skip-if-cached
        │
        ├── Light property change → LightGpuChanged{objectUid}
        │     └── Editor::LightGpuChanged handler:
        │         resolves the pooled Light*, calls UploadLighting(*light),
        │         calls RenderCache::UpdateLight(id, gpuStruct)
        │         → single SSBO struct updated
        │
        └── Light transform/visibility/shadow toggle → LightingRebuild{}
              └── Editor::LightingRebuild handler:
                  calls UploadLighting() → full light SSBO dict rebuilt
                  → all light SSBOs re-uploaded
```

The key invariant: **SceneController never touches GPU resources**. It mutates
scene objects and emits EditorEvents. Editor handles all GPU uploads in its
event handlers. This keeps the controller stateless and preserves the Editor→Renderer GPU upload boundary.
