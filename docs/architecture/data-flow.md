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

- `SceneContext` (`src/scene/SceneContext.h`) - the frame's scene view, produced
  once by `Editor::GetContext()` and embedded **by value** as `snapshot` into
  both `RenderContext` and `UIContext`
- `UIContext` - per-frame UI snapshot: the embedded `snapshot` plus typed
  profile/log/history pointers
- Read-only access to scene data for Renderer
- Data flows: Editor mutates, Renderer consumes
- **Every context member is a typed pointer declared by forward declaration in
  the context's own header** (`const Scene*`, `const RenderConfig*`,
  `const Camera*`, `const DebugDrawList*`, `const GizmoDrawList*`;
  `const FrameProfile*`, `const LogBuffer*`, `const HistoryView*` in
  `UIContext`), and **growth is by composition** - a sibling struct embedded by
  value next to `snapshot` - never by a `void*` slot. A forward declaration adds
  no include, so the typed pointer costs the layer isolation nothing; the type
  erasure it replaced cost 18 `static_cast` sites and two wrong null policies.
- `RenderConfig` (`src/core/RenderConfig.h`) is owned by the Editor directly
  (no longer via Project); passes and panels read it through
  `ctx.snapshot.Settings()`, which never returns null and falls back to
  `RenderConfig`'s own in-class member initializers - the single source of truth
  for render-setting defaults
- Null policy: `config` is always safe through `Settings()`; `scene`, `camera`,
  `debugDraw` and `gizmoDraw` MUST be checked (`HasScene()`, `HasCamera()`, or a
  plain null test)
- `Editor::SetRenderConfig()` writes UI config changes directly to the
  Editor-owned RenderConfig -> SceneContext
