/**
 * @file TransformGizmo.h
 * @brief Modal transform state machine for the viewport's G / R / S gestures.
 *
 * The gesture is armed by a mode key and then reads the bare cursor, so nothing
 * has to be grabbed for it to run. An armed gesture with no axis is a *free*
 * gesture: the view plane for Move, uniform for Scale, the view axis for
 * Rotate. It yields exactly one undo entry on confirm. This class holds no
 * operation sink, no event queue and no scene pointer: it is pure interaction
 * math over a Transform3D handed in per call.
 *
 * The arm target is a uid, not a Transform3D*: an object can be deleted
 * mid-gesture, so the caller re-resolves the uid per event and hands in the
 * reference rather than caching a pointer that could dangle.
 *
 * Every drag recomputes the transform from the *before* snapshot plus the
 * current cursor, never from the value it wrote last frame, so a cursor round
 * trip lands back on the exact starting value and an axis switch mid-gesture is
 * well defined.
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
 * Both the `before` endpoint of the undo operation and the invariant every drag
 * re-derives from; storing the whole triple makes Cancel() a literal restore.
 */
struct TransformSnapshot
{
	glm::vec3 position{0.0f};  ///< World position.
	glm::vec3 rotation{0.0f};  ///< Euler degrees (pitch=X, roll=Y, yaw=Z).
	glm::vec3 scale{1.0f};     ///< Per-axis scale.
};

/**
 * @brief The world-space unit axis a (mode, axis) pair manipulates.
 * @param rotationDegrees Transform3D::GetRotation() - the stored Euler triple.
 * @return Unit vector, or (0,0,0) when either enum is None.
 *
 * Rotate uses the gimbal axes (Z -> world Z, X -> Rz*X, Y -> Rz*Rx*Y); Move and
 * Scale use the true local axes. Derived from the stored Euler angles, never
 * from mat3(model)'s columns, whose scale makes axes vanish or flip.
 */
glm::vec3 GizmoAxisDirection(GizmoMode mode, GizmoAxis axis, const glm::vec3& rotationDegrees);

/**
 * @brief The stored Euler triple as a rotation matrix: Rz(yaw) * Rx(pitch) * Ry(roll).
 * @param rotationDegrees Transform3D::GetRotation() - the stored Euler triple.
 */
glm::mat3 GizmoRotationMatrix(const glm::vec3& rotationDegrees);

/**
 * @brief The inverse of GizmoRotationMatrix: the Euler triple that rebuilds @p r.
 * @param r A pure rotation matrix (orthonormal, right-handed).
 * @return Euler degrees (pitch=X, roll=Y, yaw=Z), pitch in [-90, 90].
 *
 * Exact and closed-form. It returns the *canonical* representative, so a free
 * rotate rewrites the whole triple rather than one number, and at pitch = +-90
 * the triple is degenerate and roll is pinned to 0.
 */
glm::vec3 GizmoEulerFromRotation(const glm::mat3& r);

/**
 * @brief One modal transform gesture: its mode, its constraint, its before-state.
 *
 * The pivot is always Transform3D::GetPosition() as captured in Arm() and stays
 * constant for the whole gesture: every drag measures against it, which is what
 * makes the math drift-free. Where the guide is drawn is a separate concern.
 */
class TransformGizmo
{
public:
	TransformGizmo() = default;

	/**
	 * @brief Begin a gesture on @p objectUid, snapshotting @p target.
	 * @param cursorPx Where the mode key was pressed - the free gesture's anchor.
	 * @return false when @p mode is None or @p objectUid is 0; nothing changes.
	 *
	 * Leaves the gesture armed with no axis, which is already a live *free*
	 * gesture for all three modes. A live gesture must be Cancel()ed first;
	 * nothing is restored here.
	 */
	bool Arm(GizmoMode mode, int objectUid, const Transform3D& target, glm::vec2 cursorPx);

	/**
	 * @brief Latch the constraint axis and the grab anchor at @p cursorPx.
	 * @param axis GizmoAxis::None means *screen space* (the W key) - it drops whichever
	 *             axis is latched and returns the gesture to its free variant.
	 * @return false when the constraint is refused; the mode stays armed, axis-less.
	 *
	 * For Move it rejects an axis within ~1.8 deg of the view ray, where the
	 * closest-point solve is meaningless. Re-calling mid-gesture re-latches the
	 * anchor, so an axis switch cleanly undoes the previous axis's partial
	 * transform.
	 */
	bool Constrain(GizmoAxis axis, const EditorViewport& vp, glm::vec2 cursorPx);

	/**
	 * @brief Recompute @p target from the before-snapshot and @p cursorPx.
	 * @return true when a component of @p target actually changed.
	 *
	 * Never reads @p target's current value as an input, which is what makes the
	 * gesture drift-free. With no axis constrained this runs the *free* variant.
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

	// The unconstrained gestures. All three measure from m_anchorPx, latched in Arm().
	bool DragMoveFree(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target);
	bool DragScaleFree(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target);
	bool DragRotateFree(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target);

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
	bool  m_rotLatched = false;  ///< Is the bearing zero set? Constrain(), else first free drag.

	// Scale, and the anchor of both free gestures. The radius is re-measured against
	// the *current* pivot pixel every drag, so a camera move or a resize mid-gesture
	// cannot make the object jump.
	glm::vec2 m_anchorPx{0.0f};  ///< Cursor pixel latched in Arm(), re-latched in Constrain().
};

} // namespace neurus
