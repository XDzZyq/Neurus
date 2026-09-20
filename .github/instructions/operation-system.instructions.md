# Operation System (Undo/Redo)

## Overview

Event-replay undo/redo (Design B, group-theoretic), implemented in
`src/editor/operations/`. Operations never mutate the scene directly: `Apply()`
re-dispatches the originating scene event via `EventQueue::emitNow`, so the
existing controller handler performs the mutation. This keeps a **single
mutation path** — undo/redo exercises the same code as a live edit. A
`Phase::Replaying` guard in `OperationManager` makes `Submit()` a no-op during
replay so playback does not re-record. The guard is RAII (restored on
exception), `Replay()` catches + logs handler exceptions and returns whether
the op applied, and `Undo()`/`Redo()` only move an op onto the opposite stack
when replay succeeded — a failed replay is dropped, never enqueued as a phantom
entry.

The controller-side wiring that produces operations (gesture begin/end,
per-controller specifics) lives in
[editor.instructions.md](editor.instructions.md); this doc covers the operation
model, coalescing rules, and persistence.

## Operation model

- Operations are **absolute state-sets** (before/after endpoints by UID), not
  deltas — safe to replay across intervening changes and to no-op on stale UIDs.
- `TransitionOp<Derived, TEvent, Value>` (CRTP) covers per-object value edits;
  `Inverse()` swaps before/after. `MergeKey()`/`MergeFrom()` coalesce a
  continuous manipulation (e.g. camera drag) into one undo entry.
- **Replay is ID-only**: ops re-dispatch via
  `MakeEvent(int objectUid, const Value&)`, producing events whose `int objectUid`
  field binds directly from the stored UID. Operations NEVER resolve objects
  themselves — the receiving controller handler resolves the UID against the
  current scene/pool via its `ControllerContext` at replay time, so stale ids
  no-op in the handler (matching the old `ctx.Resolve` behavior). The manager
  therefore needs NO scene or resource access.
- `Operation::PreservesRedo()` (default false): a branching edit clears the redo
  stack. **Selection** ops (`SetSelectionOp`) override it to `true` so navigating
  the selection appends to undo *without* discarding a pending redo — safe
  because ops are absolute. Selection is scene-level SET state, so it replays via
  the absolute `SelectionChanged` event + `Selections::RestoreState`. The *live*
  selection is also persisted independently of history by `Scene::serialize`
  (as UIDs — see data-resource.instructions.md), so a reopened project restores
  its selection even with an empty undo/redo history.
- **`CompositeOp`** (in `Operation.h`, a general-purpose core op) composes any
  sequence of `Operation`s into ONE undo entry. Group-theoretic inverse: it
  replays the sequence in forward order, and `Inverse()` returns a composite of
  the reversed, individually-inverted operations (o = h·g·f ⇒ o⁻¹ = f⁻¹·g⁻¹·h⁻¹),
  so inversion is an involution. Serializes its contained ops polymorphically
  (same pattern as the manager stacks).
- **`SceneObjectAddOp`** (membership toggle) makes Add/Delete undoable. It
  stores the object UID plus an `add` flag (the `AddShaderFieldOp` convention):
  `Apply()` re-dispatches the ORIGINATING events
  `SceneObjectAddRequested` / `SceneObjectDeleteRequested`, so replay runs the
  same controller handler as a live edit; the handler's `Submit` is muted by
  `Phase::Replaying`, so playback does not re-record (the expected "Submit
  suppressed during replay" LOG). `Inverse()` flips the flag — the inverse of
  Add is Delete and vice versa. Delete NEVER removes the pooled resource: it
  only drops the scene reference, so undo re-registers from the pool without
  any reload from disk, and the operation survives project save/load (pool +
  history both persist).
  - **Note (design):** replay re-enters the forward handler (harmless — Submit
    is muted). If a handler carries gesture side effects (selection, gesture
    state) that must NOT re-run on replay, give it a dedicated restore event
    (ShaderCodeRestored convention) instead of the forward event — but keep
    ops minimal by default.
- **Debug object ops** (issue #22, `SceneOperations.h`): ten `TransitionOp`s
  covering the debug property events — `SetDebugColorOp` (`glm::vec4`),
  `SetDebugOpacityOp`, `SetDebugXRayOp`, `SetDebugLineWidthOp`,
  `SetDebugStippleOp`, `SetDebugSmoothOp`, `SetDebugPointTypeOp`,
  `SetDebugPointScaleOp`, `SetDebugProjectionModeOp` and
  `SetDebugPositionsOp`. Like the events they replay, they are **type-agnostic**:
  one op set serves `DebugLine`, `DebugPoints` and `DebugMesh`, and the handler
  resolves the UID against whichever pool holds it. `SetDebugPositionsOp` stores
  the whole `std::vector<glm::vec3>` before/after (the op layer may use glm; the
  event may not, so `MakeEvent` flattens to xyz triples) — so a cell edit, an Add
  and a Remove are all the same absolute op, and each is one undo entry.

## Bounded undo depth

`OperationManager` caps the undo stack at `kDefaultMaxUndoDepth` (256) entries;
the ctor takes an optional `maxUndoDepth` override (0 = unbounded). `Submit` and
`RestoreHistory` call `EnforceUndoLimit()`, which evicts the **oldest** entries
(front of `m_undo`) once the cap is exceeded, bounding memory over long sessions.
Redo is bounded implicitly: its entries only ever originate from undo pops.

## Coalescing gestures into one undo entry

A continuous manipulation (a slider drag, a camera orbit) fires a *stream* of
value changes but must collapse to a single undo entry. Three strategies exist:

- **Implicit merge (MergeKey):** the op declares a non-empty `MergeKey()`;
  `OperationManager::Submit` folds a same-key edit into the undo-stack top.
  Used where there is no natural press/release boundary — **scroll zoom**
  (`CameraZoomOp`, keyed `camera_zoom:<uid>`) records per-event and merges.
- **Controller-owned gesture (explicit begin/end):** the controller holds a
  small gesture state, captures the "before" endpoint on a begin event, mutates
  live during the drag WITHOUT recording, and records ONE op on the end event.
  Used where the UI has a real press/release boundary. This needs NO changes to
  `OperationManager` or `IOperationSink` — the boundaries are ordinary typed
  events flowing through the same controller chain.
- **Composite (one gesture → many primitive ops):** where one gesture spans
  several *distinct* edits, the controller records a single `CompositeOp`
  holding the primitive sequence. Scene add records
  `CompositeOp[SceneObjectAddOp({u},true), SetSelectionOp(before→{u})]` (add
  AND select = one undo entry); scene delete records
  `CompositeOp[SetSelectionOp(before→∅), SceneObjectAddOp(uids,false)]`
  (deselect AND remove every selected object in ONE batched op = one undo
  entry). Undo replays the reversed, inverted sequence.

For the concrete controller wiring (`CameraController`,
`RenderConfigController`, `ShaderController`), see
[editor.instructions.md](editor.instructions.md).

`ShaderController` records delta-only ops matching each edit event's
granularity: `SetShaderCodeOp` (before/after GLSL text), `SetShaderFieldOp`
(before/after of one whole `ShaderStruct` element — a `ShaderFieldValue` variant
over `S_IO`/`S_Uniform`/`S_Func`/`S_PushConstant`/`S_StructDef`, serialized via the
non-intrusive functions in `render/shaders/ShaderStructSerialize.h`) and
`AddShaderFieldOp` (append vs remove one default entry, keyed by a `bool add` flag
it flips in `Inverse`).
All are keyed by mesh UID + stage. Code edits use the explicit begin/end gesture
(`ShaderEditBegin`/`ShaderEditEnd`), while discrete struct/field edits record one
op immediately per change. The ops are deliberately non-mergeable (empty
`MergeKey`). Their `Apply`/replay is CPU-only — each re-emits a dedicated restore
event (`ShaderCodeRestored` / `ShaderFieldRestored` / `ShaderFieldAddRestored` /
`ShaderFieldRemoved`) that re-applies one edit dimension and bumps
`ShaderUnit::m_version`, never recompiling to SPIR-V.
Shader **Create** is recorded by the Editor (`Editor::OnCreateShader`), not the
controller, as a pool-preserving membership toggle `ShaderLinkOp`
(`{mesh UID, pooled shader UID, add}` flag - the `SceneObjectAddOp`
convention): undo drops the mesh's shader reference (the pooled `RenderShader`
is never removed), redo relinks the same pooled shader by UID. Replay dispatches
dedicated restore events (`ShaderLinkRestored` / `ShaderUnlinkRestored`,
Editor-handled - it owns the pool) so it never re-enters the forward create
handler (no reload/recompile, no duplicate pooled shader). Compile stays
non-undoable.

## Persisting the history stacks

The undo/redo stacks are saved into the project file via `HistoryComponent`
(`src/asset/components/HistoryComponent.h/cpp`), a `project::Serializable`
adapter keyed `"m_history"` that wraps `OperationManager`. `Save` writes the
`undo`/`redo` vectors of `std::unique_ptr<Operation>`; `Load` decodes them and
calls `OperationManager::RestoreHistory`. `Application::BuildProject` registers
it **last**, after Scene/Config/UI, so a legacy file with no `m_history` node
still loads — `HistoryComponent::Load` catches the cereal exception and clears
the stacks rather than throwing.

Operations serialize polymorphically through cereal, mirroring scene objects
(`src/scene/registrations/TypeRegistration.cpp`). Each op has a default ctor and
a templated `serialize`; concrete types are registered in
`src/editor/operations/registrations/OperationRegistration.cpp` via
`CEREAL_REGISTER_TYPE` +
`CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, …)`. Two non-obvious
requirements when adding a new op to that file:

- The registration TU **must** `#include <cereal/archives/json.hpp>` before the
  macros, otherwise the type binds to no archive and save throws "Trying to save
  an unregistered polymorphic type" at runtime despite compiling cleanly.
- Ops live in a static lib, so the registration TU is dead-stripped unless
  force-linked: `CEREAL_REGISTER_DYNAMIC_INIT(neurus_operations)` in the `.cpp`
  plus `CEREAL_FORCE_DYNAMIC_INIT(neurus_operations)` (in
  `operations/registrations/OperationRegistration.h`) included at every
  serialization site.

> **Why no `polymorphic_name` on scene objects but yes on ops?** cereal only
> writes a type name when a pointer's runtime type differs from its declared
> static type. Scene pools hold concrete-typed pointers (`shared_ptr<Camera>`,
> etc.), so cereal takes the exact-type fast path (`polymorphic_id` sentinel
> `0x40000000`, no name). Operation stacks hold `unique_ptr<Operation>` (abstract
> base) with derived runtime types, so cereal records `polymorphic_name` for
> dispatch on load.
>
> Because scene pools are concrete-typed, `TypeRegistration.cpp` is never
> odr-used and is currently dropped from the final binaries by the static
> linker — the project round-trip tests pass without it. It is kept as the
> registration reference for scene types; if scene objects are ever serialized
> through `shared_ptr<ObjectID>` base pointers, it will need the same
> `CEREAL_REGISTER_DYNAMIC_INIT`/`CEREAL_FORCE_DYNAMIC_INIT` treatment as the
> ops.

## Adding a new operation

1. Give the op a default ctor + templated `serialize`.
2. Add the two registration lines in `operations/registrations/OperationRegistration.cpp`
   (`CEREAL_REGISTER_TYPE` + `CEREAL_REGISTER_POLYMORPHIC_RELATION`).

No manual type-tag or factory is needed.
