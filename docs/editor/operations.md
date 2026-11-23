# Operations

Neurus implements undo/redo as event-replay (the group-theoretic "Design B"),
living in `src/editor/operations/`.

## Overview

A live edit starts with a typed request. The controller makes the requested
change to the CPU document and calls `IOperationSink::Submit` with a description
of its effect. For example, replacing an input connection records the unlink
and link as two children of one history entry.

The operation system owns the history stacks and the open scope's buffer. Only
`OperationManager` can construct a production `OperationContext`. Undo, redo,
and rollback call `Operation::Apply` inside the operation system. Each
operation emits a typed event through that context. Its handler resolves the
target and updates the document. During undo and redo the manager suppresses
recording so that replay leaves exactly one entry on the destination stack.

The controller-side wiring that produces operations (gesture begin/end,
per-controller specifics) lives in [Controllers](controllers.md); this page
covers the operation model, coalescing rules, and persistence.

## One undo step per script run

A script run is a burst of ordinary writes: each one reaches its controller and
`Submit` exactly like a panel edit. Left alone, that would fragment a run into one
undo entry per property set, so the manager tracks the recording phase explicitly:

```cpp
enum class Phase { Idle, Replaying, Script };
```

`Idle` records normally. `Replaying` drops `Submit` (undo/redo re-runs the same
handlers). `Script` appends `Submit` to a buffer instead of the undo stack, and it
does so before the merge logic, so a write inside a run never folds into an entry
that already exists - nothing merges across a run boundary.

The run boundary is carried by two events. `ScriptEngineImpl::RunSource` emits
`ScriptRunStarted` before it executes the source and `ScriptRunFinished` on every
exit path; `ScriptController` forwards both to `BeginScriptScope()` and
`EndScriptScope()`:

- `BeginScriptScope` logs and returns when a phase is already active - a nested or
  overlapping scope has no meaningful owner - and only otherwise clears the buffer
  and enters `Phase::Script`. A refused call is not harmless: every later `Submit`
  keeps buffering into the still-open scope, so the next successful
  `EndScriptScope` commits two runs as a single undo step.
- For graph and pass-shader edits, the operation manager uses its buffer to request compilation of the affected stages before a graph read or at scope completion. It requests validation of the final graph before publication. If a request or final validation fails, the operation system replays the inverse graph edits in reverse order. The caller receives the failure even if the script caught an earlier request error.
- After validation, `EndScriptScope` wraps the remaining operations in a
  `ScriptRunOp` and records it as one undo entry. The history name, redo handling,
  and undo-depth cap follow the same rules as a standalone edit.
- An empty run records nothing, so a run that only reads cannot leave an entry
  that makes `Ctrl+Z` appear to do nothing.
- A run that raises still closes its scope. Graph and pass-shader changes roll back. Ordinary scene edits completed before the exception stay together as one undo entry.
- `Clear()` drops any buffered script operations along with both stacks.

The buffered operations are not replayed from the buffer: the committed
`ScriptRunOp` is an ordinary history entry, so undo and redo replay it like any
other composite. It carries the run name as its label, defaulting to "Run Script"
when the host supplied none, and serializes that name with the edits, so the
label is the same in the undo list, the redo list and after a save/reload. See also the write path in
[Scripting](../script/overview.md#one-undo-step-per-run).

## Operation model

- Operations are absolute state-sets (before/after endpoints by UID), not deltas:
  safe to replay across intervening changes and to no-op on stale UIDs.
- `TransitionOp<Derived, TEvent, Value>` (CRTP) covers per-object value edits;
  `Inverse()` swaps before/after. `MergeKey()`/`MergeFrom()` coalesce a continuous
  manipulation (such as a camera drag) into one undo entry.
- Replay is ID-only: ops re-dispatch via `MakeEvent(int objectUid, const Value&)`,
  producing events whose `int objectUid` field binds directly from the stored UID.
  Operations never resolve objects themselves; the receiving controller handler
  resolves the UID against the current scene/pool via its `ControllerContext` at
  replay time, so stale ids no-op in the handler (matching the old `ctx.Resolve`
  behavior). The manager therefore needs no scene or resource access.
- `Operation::PreservesRedo()` (default false): a branching edit clears the redo
  stack. Selection ops (`SetSelectionOp`) override it to `true` so navigating the
  selection appends to undo without discarding a pending redo, which is safe
  because ops are absolute. Selection is scene-level SET state, so it replays via
  the absolute `SelectionChanged` event + `Selections::RestoreState`. The live
  selection is also persisted independently of history by `Scene::serialize` (as
  UIDs), so a reopened project restores its selection even with an empty
  undo/redo history.
- `SetActiveCameraOp` (`SceneOperations.hpp`) makes "look through this camera"
  undoable. Like `SetSelectionOp` it is scene-level (not per-object) state, so it
  derives from `Operation` directly rather than from `TransitionOp`, stores the
  before/after camera UIDs (`0` = no scene camera, which hands the view back to
  the viewport's own camera), and `Apply()` does
  `Emit(ActiveCameraChanged{after})`, the same event the PropertyPanel
  checkbox emits, so there is no replay-only path. `Inverse()` swaps the
  endpoints. It does not override `PreservesRedo()`: changing the view camera is a
  real edit that branches history, unlike selection. The handler records it only
  when the value actually changed, so a checkbox toggled to the state it already
  holds is not an undo entry. The activation UID itself is persisted independently
  of history by `Scene::serialize`, so a reopened project looks through the camera
  it was saved with even with an empty undo stack.
- `CompositeOp` (in `Operation.hpp`, a general-purpose core op) composes any
  sequence of `Operation`s into one undo entry. Group-theoretic inverse: it replays
  the sequence in forward order, and `Inverse()` returns a composite of the
  reversed, individually-inverted operations (`o = h*g*f` implies
  `o^-1 = f^-1 * g^-1 * h^-1`), so inversion is an involution. It serializes its
  contained ops polymorphically (same pattern as the manager stacks).
- Render graph operations describe the affected node, connection, output tag or shader assignment. A connection identifies its source and destination sockets. Replacing a producer is a composite with two children in execution order, unlink the old producer and link the new one. Undo inverts these children in reverse order. That composite is one history entry for a standalone request and one child of a script's history entry when grouped.
- `SceneObjectAddOp` (membership toggle) makes Add/Delete undoable. It stores the
  object UID plus an `add` flag (the `AddShaderFieldOp` convention): `Apply()`
  re-dispatches the originating events `SceneObjectAddRequested` /
  `SceneObjectDeleteRequested`, so replay runs the same controller handler as a
  live edit; the handler's `Submit` is muted by `Phase::Replaying`, so playback
  does not re-record (the expected "Submit suppressed during replay" LOG).
  `Inverse()` flips the flag: the inverse of Add is Delete and vice versa. Delete
  never removes the pooled resource, it only drops the scene reference, so undo
  re-registers from the pool without any reload from disk, and the operation
  survives project save/load (pool + history both persist).
  - Design note: replay re-enters the forward handler (harmless, since `Submit` is
    muted). If a handler carries gesture side effects (selection, gesture state)
    that must not re-run on replay, give it a dedicated restore event (the
    `ShaderCodeRestored` convention) instead of the forward event, but keep ops
    minimal by default.
- Debug object ops (`SceneOperations.hpp`): nine `TransitionOp`s covering the debug
  property events: `SetDebugColorOp` (`glm::vec4`), `SetDebugOpacityOp`,
  `SetDebugXRayOp`, `SetDebugLineWidthOp`, `SetDebugStippleOp`,
  `SetDebugPointTypeOp`, `SetDebugPointScaleOp`, `SetDebugProjectionModeOp` and
  `SetDebugPositionsOp`. Like the events they replay, they are type-agnostic: one
  op set serves `DebugLine`, `DebugPoints` and `DebugMesh`, and the handler
  resolves the UID against whichever pool holds it. `SetDebugPositionsOp` stores
  the whole `std::vector<glm::vec3>` before/after (the op layer may use glm; the
  event may not, so `MakeEvent` flattens to xyz triples), so a cell edit, an Add
  and a Remove are all the same absolute op, and each is one undo entry.

## Bounded undo depth

`OperationManager` caps the undo stack at `kDefaultMaxUndoDepth` (256) entries;
the ctor takes an optional `maxUndoDepth` override (0 = unbounded). `Submit` and
`RestoreHistory` call `EnforceUndoLimit()`, which evicts the oldest entries
(front of `m_undo`) once the cap is exceeded, bounding memory over long sessions.
Redo is bounded implicitly: its entries only ever originate from undo pops.

## Coalescing gestures into one undo entry

A continuous manipulation (a slider drag, a camera orbit) fires a stream of value
changes but must collapse to a single undo entry. Three strategies exist:

- Implicit merge (`MergeKey`): the op declares a non-empty `MergeKey()`;
  `OperationManager::Submit` folds a same-key edit into the undo-stack top. Used
  where there is no natural press/release boundary: scroll zoom (`CameraZoomOp`,
  keyed `camera_zoom:<uid>`) records per-event and merges.
- Controller-owned gesture (explicit begin/end): the controller holds a small
  gesture state, captures the "before" endpoint on a begin event, mutates live
  during the drag without recording, and records one op on the end event. Used
  where the UI has a real press/release boundary. This needs no changes to
  `OperationManager` or `IOperationSink`; the boundaries are ordinary typed events
  flowing through the same controller chain.
- Composite (one gesture to many primitive ops): where one gesture spans several
  distinct edits, the controller records a single `CompositeOp` holding the
  primitive sequence. Scene add records
  `CompositeOp[SceneObjectAddOp({u},true), SetSelectionOp(before->{u})]` (add and
  select = one undo entry); scene delete records
  `CompositeOp[SetSelectionOp(before->after), SceneObjectAddOp(uids,false)]`
  (remove the listed objects in one batched op = one undo entry; the selection
  entry is skipped when the delete left it unchanged, and it comes first so undo
  restores the exact pre-delete selection after the objects are re-added).
  Undo replays the reversed, inverted sequence.

For the concrete controller wiring (`CameraController`, `RenderConfigController`,
`ShaderController`), see [Controllers](controllers.md).

`TransformGizmoController` is the controller-owned-gesture pattern at its purest:
`Arm` captures the before-state, every `Drag` writes `Transform3D` directly and
records nothing, and confirm submits exactly one `SetPositionOp`/`SetRotationOp`/
`SetScaleOp` for the whole gesture, or none at all when the value is unchanged,
which is also what makes a cancelled gesture leave no entry behind. Routing the
drag through `PositionChanged`/`RotationChanged`/`ScaleChanged` instead would
record roughly 60 entries per second: `ObjectController` submits on every one of
those events, and those three ops declare no `MergeKey()`, so nothing would
coalesce. The cost of bypassing `ObjectController` is that this controller must
emit `SceneModified` + `RenderResetEvent` itself, plus `LightGpuChanged` for a
light, whose GPU position lives in an SSBO the transform write does not touch.

`ShaderController` records delta-only ops matching each edit event's granularity:
`SetShaderCodeOp` (before/after GLSL text), `SetShaderFieldOp` (before/after of
one whole `ShaderStruct` element, a `ShaderFieldValue` variant over
`S_IO`/`S_Uniform`/`S_Func`/`S_PushConstant`/`S_StructDef`, serialized via the
non-intrusive functions in `asset/data/shader/ShaderStructSerialize.hpp`) and
`AddShaderFieldOp` (append vs remove one default entry, keyed by a `bool add` flag
it flips in `Inverse`). All are keyed by mesh UID + stage. Code edits use the
explicit begin/end gesture (`ShaderEditBegin`/`ShaderEditEnd`), while discrete
struct/field edits record one op immediately per change. The ops are deliberately
non-mergeable (empty `MergeKey`). Their `Apply`/replay is CPU-only: each re-emits
a dedicated restore event (`ShaderCodeRestored`/`ShaderFieldRestored`/
`ShaderFieldAddRestored`/`ShaderFieldRemoved`) that re-applies one edit dimension
and bumps `ShaderUnit::m_version`, never recompiling to SPIR-V.

Shader Create is recorded by the Editor (`Editor::OnCreateShader`), not the
controller, as a pool-preserving membership toggle `ShaderLinkOp` (`{mesh UID,
pooled shader UID, add}` flag, the `SceneObjectAddOp` convention): undo drops the
mesh's shader reference (the pooled `RenderShader` is never removed), redo relinks
the same pooled shader by UID. Replay dispatches dedicated restore events
(`ShaderLinkRestored`/`ShaderUnlinkRestored`, Editor-handled, since it owns the
pool) so it never re-enters the forward create handler (no reload/recompile, no
duplicate pooled shader). Compile stays non-undoable.

## Persisting the history stacks

The undo/redo stacks are saved into the project file via `HistoryComponent`
(`src/editor/project/HistoryComponent.hpp/cpp`), a `project::Serializable` adapter
keyed `"m_history"` that wraps `OperationManager`. `Save` writes the `undo`/`redo`
vectors of `std::unique_ptr<Operation>`; `Load` decodes them and calls
`OperationManager::RestoreHistory`. `Application::BuildProject` registers it last,
after Scene/Config/UI, so a legacy file with no `m_history` node still loads:
`HistoryComponent::Load` catches the cereal exception and clears the stacks rather
than throwing.

Operations serialize polymorphically through cereal, mirroring scene objects
(`src/scene/registrations/TypeRegistration.cpp`). Each op has a default ctor and a
templated `serialize`; concrete types are registered in
`src/editor/operations/registrations/OperationRegistration.cpp` via
`CEREAL_REGISTER_TYPE` + `CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, ...)`.
Two non-obvious requirements when adding a new op to that file:

- The registration TU must `#include <cereal/archives/json.hpp>` before the
  macros, otherwise the type binds to no archive and save throws "Trying to save
  an unregistered polymorphic type" at runtime despite compiling cleanly.
- Ops live in a static lib, so the registration TU is dead-stripped unless
  force-linked: `CEREAL_REGISTER_DYNAMIC_INIT(neurus_operations)` in the `.cpp`
  plus `CEREAL_FORCE_DYNAMIC_INIT(neurus_operations)` (in
  `operations/registrations/OperationRegistration.hpp`) included at every
  serialization site.

Why no `polymorphic_name` on scene objects but yes on ops? cereal only writes a
type name when a pointer's runtime type differs from its declared static type.
Scene pools hold concrete-typed pointers (`shared_ptr<Camera>`, etc.), so cereal
takes the exact-type fast path (`polymorphic_id` sentinel `0x40000000`, no name).
Operation stacks hold `unique_ptr<Operation>` (abstract base) with derived runtime
types, so cereal records `polymorphic_name` for dispatch on load.

Because scene pools are concrete-typed, `TypeRegistration.cpp` is never odr-used
and is currently dropped from the final binaries by the static linker; the project
round-trip tests pass without it. It is kept as the registration reference for
scene types; if scene objects are ever serialized through `shared_ptr<ObjectID>`
base pointers, it will need the same
`CEREAL_REGISTER_DYNAMIC_INIT`/`CEREAL_FORCE_DYNAMIC_INIT` treatment as the ops.

## Adding a new operation

1. Handle the live request in its controller and submit a record of the change. Implement replay using a typed event. Give the operation a default constructor and a templated `serialize` method.
2. Add the two registration lines in
   `operations/registrations/OperationRegistration.cpp`
   (`CEREAL_REGISTER_TYPE` + `CEREAL_REGISTER_POLYMORPHIC_RELATION`).

No manual type-tag or factory is needed.
