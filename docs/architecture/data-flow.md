# Data Flow

All cross-layer traffic follows a single unidirectional chain, carried by Qt
signals, typed events and read-only context objects.

## Isolation Chain

Data and control flow follow this strict unidirectional chain:

```
UI Layer (Qt6) -> UIEvents (Qt Signals) -> Editor -> EventQueue (typed events) -> Controllers -> Context -> Renderer
```

- UI emits signals via UIEvents. It NEVER calls the Editor or Renderer directly.
- Editor receives raw InputState, translates it to typed events via EventQueue,
  and dispatches to Controllers.
- Controllers handle discrete events (for example CameraController subscribes to
  CameraEvents) - no per-frame polling.
- Renderer receives read-only scene data through Context objects. It NEVER calls
  back into the Editor.
- Each arrow represents a **decoupled interface** (signals, events, contexts).
  No layer knows the internals of the next.

## Three Event Paths

Event structs in `src/editor/events/` are split by domain:
`SceneEvents.h` (scene-domain events carrying plain `int objectUid`: selection,
transform, visibility, camera/mesh/light/env property edits), `EditorEvents.h`
(cross-component events: RenderResetEvent, EnvironmentChanged, SceneModified,
LightGpuChanged, LightingRebuild, SceneObjectGpuUploadRequested), and
`AssetEvents.h` (asset add/import: mesh/camera/light adds). Scene mutations are
handled by `SceneController`, which emits `EditorEvents` for GPU uploads and
dirty tracking; the Editor executes those uploads.

Cross-layer traffic flows along three distinct paths, all still "UI emits,
Editor wraps or forwards, controllers handle":

1. **UI -> Editor pure intents** (`InputEvents.h`): the raw input stream -
   mouse move/press/release/scroll plus
   `ObjectClicked { int objectUid; int modifiers; }` and `DeleteRequested { }`.
   These carry no scene; the UI does not hold or stamp the scene.
2. **UI -> Controller editing events** (`SceneEvents.h`, `ShaderEvents.h`):
   complete scene mutations (selection, transform, visibility, property edits,
   shader edits) carrying `int objectUid`. Panels emit these with the active
   object's integer UID; `Editor::OnUIEvent` enqueues them unchanged and
   controllers resolve the UID against the current scene via their
   `ControllerContext`.
3. **Editor -> Controller wrapped events**: `ObjectSelected` (the Editor
   subscribes to `ObjectClicked` and forwards
   `ObjectSelected { objectUid, modifiers }`, and the Application emits it for
   the viewport IDBuffer pick), `ObjectDeleteRequested` (the Editor subscribes
   to `DeleteRequested` and forwards `ObjectDeleteRequested { }`), the
   membership events (`SceneObjectAddRequested` / `SceneObjectDeleteRequested`),
   and the replay / restore events. `CameraEvents` carry `int camId` (resolved
   by the controller).

On the UI->Editor path the UI emits PURE INPUT INTENTS (`InputEvents.h`) that
carry no scene; the Editor wraps them into dedicated scene events
(`ObjectSelected`, `ObjectDeleteRequested`), and controllers handle complete
scene mutations carrying `int objectUid`, resolving the UID to the live object
via their `ControllerContext` (never raw pointers - a UID stays stable after
deletion while a pointer would dangle). The scene itself is not carried in
events: scene-scoped handlers obtain it from the context.

## UIEvents System (Qt Signals)

- QObject singleton with typed Qt signals
- UI panels emit their own signals (for example `Outliner::objectClicked`,
  `RenderConfigPanel::configValueChanged`)
- `Application::ConnectUIEvent<T>` template bridges panel signals ->
  `Editor::OnUIEvent<T>` -> `EventQueue::enqueue<T>`

## EventQueue System (Typed Event Dispatcher)

- Header-only template-based event dispatcher (no Qt dependency)
- Editor<->Renderer event dispatch with deferred `Process()`

## Context System (Data)

Two frame views over the same document state, with the Editor as the single
pivot, so neither consumer reaches around it for the other's data:

- `RenderContext` (`src/render/RenderContext.h`) - the renderer's view. The
  Editor half is a single owned frame snapshot: the Application publishes one
  `RenderFrameSnapshot` per frame (built by `Editor::BuildSnapshot()`) and points
  `ctx.snapshot` at it; `DeferredRenderer` then stamps the frame half (`width`,
  `height`, `frameIndex`, `jitter`, `iteration`) through `recordFrame()`. There
  is no second publication path - a pass reads what is being rendered through
  `ctx.Frame()`.
- `UIContext` (`src/ui/UIContext.h`) - the UI's view, two fields wide: `scene`
  and `config` from `Editor::UpdateUIContext()`, plus the Application's typed
  `profile`/`log`/`history` pointers. A panel never names the camera or either
  overlay payload.
- Read-only access to scene data for Renderer
- Data flows: Editor mutates, Renderer consumes
- **`RenderContext` carries the Editor half as one forward-declared
  `const RenderFrameSnapshot*`** (`src/render/RenderFrameSnapshot.h`) - an owned
  deep copy with no Scene pointers and no lazy reads, so the renderer reads values
  the Editor is free to keep mutating. `UIContext`'s members stay typed pointers
  declared by forward declaration (`const Scene*`, `const RenderConfig*`,
  `const FrameProfile*`, `const LogBuffer*`, `const HistoryView*`), and **growth
  is by a new typed field on whichever view owns its source** - never by a
  `void*` slot. A forward declaration adds no include, so the typed pointer costs
  the layer isolation nothing; the type erasure it replaced cost 18 `static_cast`
  sites and two wrong null policies.
- `RenderConfig` (`src/core/RenderConfig.h`) is owned by the Editor directly
  (no longer via Project); `BuildSnapshot()` copies it into the snapshot, where a
  pass reads it through `ctx.Frame().Settings()`, and a panel reads the Editor's
  copy through `UIContext::Settings()`. Neither returns null - both fall back to
  `DefaultRenderConfig()`, the single source of truth for render-setting defaults,
  so a pass and a panel cannot disagree about an unset value
- Null policy: `ctx.Frame()` itself is never null - an unpublished context reads
  as an empty frame (no scene, no camera, no draws, default settings), so what a
  pass used to null-check on the context it now checks on the frame
  (`ctx.Frame().HasScene()`, `HasCamera()`, `HasDebugDraw()`). On `UIContext`,
  `config` is always safe through `Settings()` while `scene` MUST be checked
  (`HasScene()`)
- `Editor::SetRenderConfig()` writes UI config changes directly to the
  Editor-owned RenderConfig, which then reaches the renderer in the next frame's
  snapshot and the UI through `UpdateUIContext()`
