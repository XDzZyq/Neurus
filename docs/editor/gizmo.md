# Transform Gizmo

Neurus's modal transform handle (Blender-style G / R / S) is a separate system
from the debug overlay: it is transient editor UI, not scene content.

## A gizmo handle is not a scene object

A gizmo handle must not reuse `DebugLine` / `DebugPoints` / `DebugMesh`. It is not
pooled, not serialized, not selectable, and never appears in the Outliner.
`GizmoDrawBuilder` synthesizes a fresh `GizmoDrawList` from `TransformGizmo`
state; both share `OverlayGeometry.h`'s records with `DebugDrawList` and nothing
else.

Blender-style modal transform: `G` / `R` / `S` arm a gesture on the active
object, `X` / `Y` / `Z` constrain it, `W` drops back to screen space, LMB or
Return confirms, and Esc / RMB / viewport focus loss cancels.

## Two members, one seam

Two `EditorViewport`-owned members, split by responsibility and both Qt-free and
Vulkan-free:

| Member | Role |
|---|---|
| `v_gizmo` (`TransformGizmo`) | the state machine: what is armed, on what, since what snapshot |
| `v_gizmoDraw` (`GizmoDrawBuilder`) | the picture: state + viewport -> `GizmoDrawList` |

Neither is a controller. `TransformGizmoController` is the controller and owns
nothing: it reaches the state machine through its owner,
`ControllerContext::viewport()->Gizmo()`. See [Controllers](controllers.md) and
[Events](events.md).

## Every drag derives from the snapshot

Every drag derives from the gesture's snapshot, never from the value written last
frame. `Arm()` takes `m_before` once; `Drag()` computes the transform as a
function of `(m_before, cursor)` alone. Accumulating deltas would drift by
construction and would make a cursor round trip land near, not on, the starting
value. Because nothing accumulates, a return to the anchor pixel reproduces
`m_before` bitwise, which is what `test_transform_gizmo.cpp` asserts with `==`
rather than a tolerance.

Re-constraining normalizes back to the before-state, and that is the whole of
`W`. `Constrain()` re-latches its anchors at the current cursor and the controller
then issues one `Drag()` at that same cursor, which must reproduce `m_before`
exactly. An axis switch therefore undoes the previous axis's partial transform
with no special case, and `W` is just `GizmoAxis::None` travelling the same path.

## Two axis sets

The model matrix is `T * Rz(yaw) * Rx(pitch) * Ry(roll) * S`.

Rotate uses the gimbal set (`Z` -> world Z, `X` -> `Rz*X`, `Y` -> `Rz*Rx*Y`), so a
constrained rotation's PropertyPanel number stays exactly `before + theta` with no
matrix round trip. Move and Scale use the true local set (the columns of `R`). The
two share Y always, X only when `roll == 0`, and Z only when pitch and roll are
both zero (a pure pitch already separates the two Z axes).

Both sets are derived from the stored Euler triple, never from `mat3(model)`'s
columns, which carry scale and would hand back a zero axis on a zero scale
component and a flipped one on a mirrored object.

## Free rotate

A free rotate turns about the view axis and is composed as `R_view(theta) *
R_before`, then read back through `GizmoEulerFromRotation` (the ZXY extraction).
That extraction returns the canonical representative, so a stored yaw of 200
degrees comes back as -160: the same matrix, different numbers in the panel. Hence
the zero-angle case writes `m_before.rotation` back rather than returning early: an
unturned gesture must not silently canonicalise the triple.

The closed-form derivation of `GizmoEulerFromRotation` (expanding `Rz*Rx*Ry = r`
and transposing the index order) is no longer written anywhere; only the note that
glm's `r[col][row]` is transposed remains.

## Guide geometry is a fixed pixel budget

Guide geometry is a fixed pixel budget, so the camera is an input. Every length in
`GizmoDrawBuilder` is a constant in logical pixels divided by
`PixelsPerWorldUnit(vp, depth)`, which is what holds the handle's apparent size as
the camera dollies, and is why a camera change must `MarkGizmoDirty()` even though
the state machine has not moved (`HandleResize` does exactly that). `Rebuild()`
early-returns when clean and the builder starts dirty. It runs inside
`RebuildOverlays()`, which `Edit()` calls after the queue drains and after the
second `SyncCamera()`, for exactly that reason. `RebuildOverlays()` reports
whether either payload was rebuilt, and that is what makes `Edit()` post a
`RedrawOverlaysRequest` - a camera move already restarts accumulation through
`RenderResetEvent`, but a selection change does not, and the converged renderer
would otherwise keep presenting the old guide. See [Viewport](viewport.md).

## The anchor

The anchor is the live transform; only part of it is followed.
`EditorViewport::GizmoAnchor` resolves the gesture's own uid while one is armed
(the Outliner can change the selection mid-gesture, and following that would tear
the guide off the object being transformed) and the selection's active object
otherwise. The builder then decides what to honour: the pivot rides the live
position, so a Move drags its own handle along, but the rotation does not, since
guides that spun with the object would destroy the reference the rotation is being
measured against. An armed gesture whose target was deleted mid-drag falls back to
its frozen snapshot rather than blinking out.

## Empty, never null

The viewport publishes `&m_gizmoDraw.List()` unconditionally through
`GizmoList()`; "nothing armed and nothing selected" is an empty list, and
`GizmoPass` early-outs on null and empty alike. Counts are the only handle a CPU
test has on which mode is live (a Move guide is 9 segments, a Scale guide is 1 + 2
sprites, a Rotate ring is 48 chords), and `test_gizmo_draw_builder.cpp` asserts
over counts, predicates and the longest segment, never an index, because primitive
order is an implementation detail.

## Dimming

Dimming is RGB, never alpha (`kCandidateDim`): a translucent guide washes out over
bright geometry. The only alpha the gizmo uses is `OverlayFlag::Smooth`'s analytic
edge fade, which is antialiasing rather than transparency, so every primitive in
every state is fully opaque.

## One gesture, one undo entry

A whole gesture is exactly one undo entry, or none when nothing moved. The drag
writes `Transform3D` directly and records nothing, so the controller notifies for
itself through `neurus::notify`, the mutation policy it shares with
`SceneController`. Two tiers: a drag frame is `ObjectPreviewed` (cheap light sync
plus repaint, and never `SceneModified`, which is one-way, so a cancelled gesture
must not set it), and a confirm is `ObjectMutated` (dirty plus the full light-dict
rebuild). On confirm the controller submits exactly one
`SetPositionOp`/`SetRotationOp`/`SetScaleOp` for the whole gesture. The cancel path
is itself a preview, which is why a whole G-drag-Esc round trip leaves no trace.
See [Operations](operations.md) and [Controllers](controllers.md).

## Draggable handles are a sanctioned future feature

Draggable handles are a sanctioned future feature, not an excluded one. The
pieces it needs are already in place: `EditorViewport::Project` for a
pixel-distance-to-segment hit test (CPU-analytic, no GPU handle-ID buffer), press
precedence ahead of the IDBuffer pick, and drag-and-release mapped onto the
existing confirm.

## Rendering

`GizmoPass` runs last, after `DebugPass`, into the same overlay image
(`OverlayOutput`) with `LOAD_OP_LOAD` and no depth attachment at all: every guide
is unconditionally x-ray, so there is no partition and exactly two draws. The
overlay image, not the pass's target, is what the swapchain blit reads.
