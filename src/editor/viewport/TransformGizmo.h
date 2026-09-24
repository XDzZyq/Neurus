/**
 * @file TransformGizmo.h
 * @brief Modal transform state machine for the viewport's G / R / S gestures.
 *
 * Blender-style modal operators rather than draggable handles: the *gesture* does
 * not exist until the user presses a mode key, so there is no handle geometry to
 * hit-test and no picking cost. A resting translate handle is drawn whenever
 * something is selected, but it is display only — GizmoDrawBuilder produces it from
 * the active object's transform without this class being armed at all, and clicking
 * it does nothing. One gesture is
 *
 *     Arm(mode) -> Constrain(axis) -> Drag(cursor)* -> Disarm() | Cancel()
 *
 * and it yields exactly one undo entry, Submitted by TransformGizmoController on
 * confirm. This class holds no operation sink, no event queue and no scene
 * pointer: it is pure interaction math over a Transform3D handed in per call.
 *
 * ## Why the target is a uid, not a Transform3D*
 *
 * An object can be deleted mid-gesture — EventQueue::Process() drains re-entrant
 * enqueues in the same call, so a delete can land between two drags of one frame.
 * A cached Transform3D* would dangle for the rest of that Edit(); a uid cannot.
 * The controller re-resolves the uid per event and hands in the reference.
 *
 * ## Drift-free by construction
 *
 * Every drag recomputes the transform from the *before* snapshot plus the current
 * cursor, never from the value it wrote last frame. A round trip of cursor
 * positions therefore lands back on the exact starting value, and a dropped or
 * duplicated mouse event cannot accumulate error. It is also what makes an
 * axis switch mid-gesture well defined: re-Constrain re-latches the anchor, and
 * the next drag is measured from the snapshot again.
 *
 * ## The cursor is a parameter, not read from the viewport
 *
 * Drag() takes the cursor explicitly because the controller must pass
 * MouseMoveEvent::position: the Editor's own subscription is what pushes the
 * cursor into EditorViewport, and EventQueue dispatch order is registration
 * order, so reading it back would silently consume a one-frame-stale cursor
 * whenever the two subscriptions were registered the other way round.
 *
 * Layer placement: editor, Vulkan-free and Qt-free. Reads Camera and Transform3D
 * (scene layer) and EditorViewport (its own layer), nothing else.
 */

#pragma once

#include <glm/glm.hpp>

#include "editor/events/GizmoEvents.h"

namespace neurus
{

class EditorViewport;
class Transform3D;

/**
 * @brief The transform as it stood when the gesture began.
 *
 * Two jobs at once, which is why it is one struct and not two: it is the `before`
 * endpoint of the undo operation, and it is the invariant every drag re-derives
 * from. Storing the whole triple rather than only the component being edited costs
 * 24 bytes and makes Cancel() a literal restore.
 */
struct TransformSnapshot
{
	glm::vec3 position{0.0f};  ///< World position.
	glm::vec3 rotation{0.0f};  ///< Euler degrees (pitch=X, roll=Y, yaw=Z).
	glm::vec3 scale{1.0f};     ///< Per-axis scale.
};

/**
 * @brief The world-space unit axis a (mode, axis) pair manipulates.
 * @param rotationDegrees Transform3D::GetRotation() — the stored Euler triple.
 * @return Unit vector, or (0,0,0) when either enum is None.
 *
 * Two different axis sets, because the model matrix is
 * T * Rz(yaw) * Rx(pitch) * Ry(roll) * S — S is innermost but R is not:
 *
 *   Rotate (the gimbal set). Bumping one stored Euler component is an exact
 *   axis-angle rotation about an axis depending only on the components *not*
 *   changed — a true one-parameter rotation subgroup. That is what lets the whole
 *   feature avoid quaternions and decomposition and keep the PropertyPanel number
 *   exactly `before + theta`:
 *       Z (yaw)   -> world Z
 *       X (pitch) -> Rz(yaw) * X
 *       Y (roll)  -> Rz * Rx * Y, which is exactly Transform3D::GetDirection()
 *
 *   Move / Scale (the true local set) — the columns of R = Rz * Rx * Ry. Only Y
 *   coincides with the gimbal set; X and Z differ whenever roll != 0. For an
 *   unrotated object all three reduce to world XYZ.
 *
 * Derived from the stored Euler angles and never from the columns of
 * mat3(model): those are R*X * scale.x and friends, so a zero scale component
 * yields a zero axis and a negative one yields a flipped axis — the drag would
 * silently run backwards on any mirrored object.
 *
 * A free function rather than a member so the derivation is unit-testable on its
 * own and so GizmoDrawBuilder can draw the two axes the gesture did *not* pick
 * without the state machine growing an accessor per axis.
 */
glm::vec3 GizmoAxisDirection(GizmoMode mode, GizmoAxis axis, const glm::vec3& rotationDegrees);

/**
 * @brief One modal transform gesture: its mode, its constraint, its before-state.
 *
 * Owned by value by the Editor, beside DebugDrawBuilder. Ten public methods, five
 * of them one-line accessors; the geometry of the guide lines lives in
 * GizmoDrawBuilder, which reads this object through those accessors and the free
 * GizmoAxisDirection() above.
 *
 * The pivot is always Transform3D::GetPosition() as captured in Arm() — never
 * MeshData::center, which is a vertex centroid and not a bbox centre — and it is
 * constant for the whole gesture: Rotate and Scale do not move the position, and
 * Move's constraint line is anchored at it.
 */
class TransformGizmo
{
public:
	TransformGizmo() = default;

	/**
	 * @brief Begin a gesture on @p objectUid, snapshotting @p target.
	 * @return false when @p mode is None or @p objectUid is 0; nothing changes.
	 *
	 * Leaves the gesture armed with no axis: Wave 1 applies no transform until an
	 * axis key arrives. Does not restore anything — a live gesture must be
	 * Cancel()ed by the controller first, which is what makes pressing R during a
	 * G discard the move, as in Blender.
	 */
	bool Arm(GizmoMode mode, int objectUid, const Transform3D& target);

	/**
	 * @brief Latch the constraint axis and the grab anchor at @p cursorPx.
	 * @return false when the constraint is refused; the mode stays armed, axis-less.
	 *
	 * The one call that can refuse. For Move it rejects an axis within ~1.8 deg of
	 * the view ray (1 - dot(axis, ray)^2 < 1e-3), where the closest-point solve is
	 * numerically meaningless and a pixel of cursor motion is metres of travel.
	 * For Rotate it also latches the screen-space sign *once*: re-deriving it per
	 * frame flips chaotically as the axis crosses edge-on.
	 *
	 * Re-calling it mid-gesture (a different axis, or the same one again) re-latches
	 * the anchor, so the very next Drag() at the same cursor reproduces the
	 * before-state exactly. That is how an axis switch cleanly undoes the partial
	 * transform the previous axis had applied.
	 */
	bool Constrain(GizmoAxis axis, const EditorViewport& vp, glm::vec2 cursorPx);

	/**
	 * @brief Recompute @p target from the before-snapshot and @p cursorPx.
	 * @return true when a component of @p target actually changed.
	 *
	 * Never reads @p target's current value as an input, which is what makes the
	 * gesture drift-free. A no-op while no axis is constrained.
	 */
	bool Drag(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target);

	/// Restore @p target to the before-snapshot. No-op when inactive.
	void Cancel(Transform3D& target);

	/// End the gesture, keeping whatever @p target currently holds.
	void Disarm();

	bool      IsActive() const { return m_mode != GizmoMode::None; }
	GizmoMode Mode() const { return m_mode; }
	GizmoAxis Axis() const { return m_axis; }
	int       ObjectUid() const { return m_objectUid; }

	/// The snapshot taken in Arm(): the undo `before` and the drag invariant.
	const TransformSnapshot& Before() const { return m_before; }

private:
	bool DragMove(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target);
	bool DragRotate(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target);
	bool DragScale(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target);

	GizmoMode m_mode = GizmoMode::None;
	GizmoAxis m_axis = GizmoAxis::None;
	int       m_objectUid = 0;

	TransformSnapshot m_before{};   ///< Captured in Arm(); every drag re-derives from it.
	glm::vec3 m_axisDir{0.0f};      ///< Unit constraint axis, latched in Constrain().

	// Move. Both are parameters along the constraint line, measured from the pivot.
	float m_grabAnchorT = 0.0f;  ///< Where the cursor grabbed the line.
	float m_lastT = 0.0f;        ///< Last valid solve; frozen on, not clamped to, degeneracy.

	// Rotate. The accumulated angle is unwrapped, so >180 deg and multiple turns work.
	float m_rotSign = -1.0f;     ///< Screen-to-world sign, latched once in Constrain().
	float m_prevAngle = 0.0f;    ///< Previous cursor bearing about the pivot, radians.
	float m_accumAngle = 0.0f;   ///< Total unwrapped rotation, radians.

	// Scale. The radius is re-measured against the *current* pivot pixel every drag,
	// so a camera move or a resize mid-gesture cannot make the object jump.
	glm::vec2 m_anchorPx{0.0f};  ///< Cursor pixel latched in Constrain().
};

} // namespace neurus
