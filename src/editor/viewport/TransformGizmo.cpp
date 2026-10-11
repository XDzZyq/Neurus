/**
 * @file TransformGizmo.cpp
 * @brief Implementation of the modal G / R / S state machine.
 *
 * Two invariants are load-bearing and easy to "tidy" into a bug: every drag
 * derives the new value from m_before rather than from the transform's current
 * value (drift-free, and an axis switch snaps back cleanly), and the Rotate
 * sign is latched once because re-deriving it per frame flips chaotically
 * whenever the axis passes edge-on.
 */

#include "editor/viewport/TransformGizmo.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "editor/viewport/EditorViewport.h"
#include "scene/Camera.h"
#include "scene/Transform.h"

namespace neurus
{

namespace
{

/// @brief Below this, the closest-point solve is meaningless: an axis within ~1.8 deg of the view ray.
constexpr float kMinAxisSeparation = 1e-3f;

/// @brief Edge-on band for the Rotate sign latch, and the zero-length vector guard.
constexpr float kEpsilon = 1e-3f;

/// @brief Screen radius floor for Rotate/Scale, in logical pixels.
/// Clamping both radii the same way makes the scale factor exactly 1 at the anchor.
constexpr float kMinRadiusPx = 8.0f;

/// @brief Smallest |scale| component we will write.
/// The normal-matrix inverse of a zero component is singular, so a collapsing
/// drag is pinned just short of zero instead.
constexpr float kMinScaleComponent = 1e-4f;

constexpr float kTwoPi = 6.283185307179586f;

/// @brief Which stored Euler component a Rotate axis bumps.
/// The identity on indices: Transform3D stores (pitch=X, roll=Y, yaw=Z).
int ComponentIndex(GizmoAxis axis)
{
	switch (axis)
	{
	case GizmoAxis::X: return 0;
	case GizmoAxis::Y: return 1;
	case GizmoAxis::Z: return 2;
	default: return 0;
	}
}

/// @brief Parameter along @p axis (through @p pivot) closest to @p ray.
/// @return false when the two lines are too close to parallel to solve.
///
/// Standard line-line closest-point solve with both directions unit length.
bool ClosestParamOnAxis(const glm::vec3& pivot, const glm::vec3& axis, const ScreenRay& ray,
                        float& outT)
{
	const float b = glm::dot(axis, ray.direction);
	const float denom = 1.0f - b * b;
	if (denom < kMinAxisSeparation)
		return false;

	const glm::vec3 w0 = pivot - ray.origin;
	outT = (glm::dot(ray.direction, w0) * b - glm::dot(axis, w0)) / denom;
	return true;
}

} // namespace

glm::mat3 GizmoRotationMatrix(const glm::vec3& rotationDegrees)
{
	// The rotation part of the model matrix, exactly: no other factor order is equivalent.
	const glm::vec3 rad = glm::radians(rotationDegrees);
	const glm::mat4 rz = glm::rotate(glm::mat4{1.0f}, rad.z, glm::vec3{0.0f, 0.0f, 1.0f});
	const glm::mat4 rx = glm::rotate(glm::mat4{1.0f}, rad.x, glm::vec3{1.0f, 0.0f, 0.0f});
	const glm::mat4 ry = glm::rotate(glm::mat4{1.0f}, rad.y, glm::vec3{0.0f, 1.0f, 0.0f});
	return glm::mat3{rz * rx * ry};
}

glm::vec3 GizmoEulerFromRotation(const glm::mat3& r)
{
	// Solving Rz(yaw) * Rx(pitch) * Ry(roll) = r; glm's r[col][row] is transposed
	// against the textbook picture, so every index below is swapped.
	const float pitch = std::asin(std::clamp(r[1][2], -1.0f, 1.0f));

	// cos(pitch) is non-negative over asin's whole range, so its own size is the only
	// degeneracy test needed -- no quadrant case analysis.
	if (std::cos(pitch) < kEpsilon)
	{
		// Gimbal lock: with sa = +-1 yaw and roll turn about the same world axis and
		// only their sum is observable. Pinning roll to 0 picks one representative.
		return glm::degrees(glm::vec3{pitch, 0.0f, std::atan2(r[0][1], r[0][0])});
	}

	// Both atan2s carry the same positive cos(pitch) factor in each argument, which
	// cancels -- so neither needs dividing through by it first.
	return glm::degrees(glm::vec3{pitch,
	                              std::atan2(-r[0][2], r[2][2]),
	                              std::atan2(-r[1][0], r[1][1])});
}

glm::vec3 GizmoAxisDirection(GizmoMode mode, GizmoAxis axis, const glm::vec3& rotationDegrees)
{
	if (mode == GizmoMode::None || axis == GizmoAxis::None)
		return glm::vec3{0.0f};

	if (mode == GizmoMode::Rotate)
	{
		// The gimbal set: the axis each stored Euler component actually turns about,
		// built from partial products rather than the full rotation matrix.
		const glm::vec3 rad = glm::radians(rotationDegrees);
		const glm::mat4 rz = glm::rotate(glm::mat4{1.0f}, rad.z, glm::vec3{0.0f, 0.0f, 1.0f});
		const glm::mat4 rx = glm::rotate(glm::mat4{1.0f}, rad.x, glm::vec3{1.0f, 0.0f, 0.0f});

		switch (axis)
		{
		case GizmoAxis::Z:  // yaw, the outermost rotation
			return glm::vec3{0.0f, 0.0f, 1.0f};
		case GizmoAxis::X:  // pitch, after yaw
			return glm::normalize(glm::vec3{rz * glm::vec4{1.0f, 0.0f, 0.0f, 0.0f}});
		case GizmoAxis::Y:  // roll, after yaw and pitch == Transform3D::GetDirection()
			return glm::normalize(glm::vec3{rz * rx * glm::vec4{0.0f, 1.0f, 0.0f, 0.0f}});
		default:
			return glm::vec3{0.0f};
		}
	}

	// Move / Scale: the true local frame, the columns of R = Rz * Rx * Ry.
	const glm::mat3 r = GizmoRotationMatrix(rotationDegrees);
	switch (axis)
	{
	case GizmoAxis::X: return glm::normalize(r[0]);
	case GizmoAxis::Y: return glm::normalize(r[1]);
	case GizmoAxis::Z: return glm::normalize(r[2]);
	default: return glm::vec3{0.0f};
	}
}

namespace
{

/**
 * @brief The screen-gesture-to-world-angle sign, read once per gesture.
 *
 * Pixel space has y down, so a right-handed rotation about an axis pointing at the
 * viewer runs the cursor bearing backwards; that case, and the edge-on band where
 * `facing` is meaningless, take -1.
 */
float LatchRotationSign(const glm::vec3& axis, const glm::vec3& pivot, const EditorViewport& vp)
{
	float facing = 0.0f;
	if (const Camera* cam = vp.GetCamera())
	{
		const glm::vec3 toCam = cam->GetPosition() - pivot;
		const float len = glm::length(toCam);
		if (len > kEpsilon)
			facing = glm::dot(axis, toCam / len);
	}

	return (facing > -kEpsilon) ? -1.0f : 1.0f;
}

/**
 * @brief Where @p cursorPx lands on the view plane through @p planePoint.
 * @return false with @p out untouched when there is no camera or the solve degenerates.
 *
 * The plane is parallel to the screen, so one pixel of cursor motion is one pixel
 * of world motion at the pivot's depth, with no foreshortening. Anchored on the
 * *before* position so the plane cannot drift as the drag carries the object off it.
 */
bool ViewPlanePoint(const EditorViewport& vp, glm::vec2 cursorPx, const glm::vec3& planePoint,
                    glm::vec3& out)
{
	const glm::vec3 n = vp.ViewAxis();
	if (glm::length(n) < kEpsilon)
		return false;  // no camera, or one looking at its own eye

	const ScreenRay ray = vp.RayThrough(cursorPx);
	if (!ray.valid)
		return false;

	const float denom = glm::dot(n, ray.direction);
	if (std::abs(denom) < kMinAxisSeparation)
		return false;  // grazing: the intersection is arbitrarily far off

	out = ray.origin + ray.direction * (glm::dot(n, planePoint - ray.origin) / denom);
	return true;
}

} // namespace

bool TransformGizmo::Arm(GizmoMode mode, int objectUid, const Transform3D& target,
                         glm::vec2 cursorPx)
{
	if (mode == GizmoMode::None || objectUid == 0)
		return false;

	m_mode = mode;
	m_objectUid = objectUid;
	m_axis = GizmoAxis::None;
	m_axisDir = glm::vec3{0.0f};

	m_before.position = target.GetPosition();
	m_before.rotation = target.GetRotation();
	m_before.scale = target.GetScale();

	m_grabAnchorT = 0.0f;
	m_lastT = 0.0f;
	m_rotSign = -1.0f;
	m_prevAngle = 0.0f;
	m_accumAngle = 0.0f;
	m_rotLatched = false;

	// The free gesture's only anchor. A key press carries no cursor of its own, so the
	// caller hands in the retained one; Constrain() re-latches it for Scale.
	m_anchorPx = cursorPx;
	return true;
}

bool TransformGizmo::Constrain(GizmoAxis axis, const EditorViewport& vp, glm::vec2 cursorPx)
{
	if (m_mode == GizmoMode::None || !vp.IsValid())
		return false;

	if (axis == GizmoAxis::None)
	{
		// W: back to the screen-space variant. Re-latching every free anchor here is
		// what makes the next drag reproduce the before-state, so dropping the
		// constraint *undoes* whatever the axis had applied.
		m_anchorPx = cursorPx;
		m_prevAngle = 0.0f;
		m_accumAngle = 0.0f;
		m_rotLatched = false;
		m_axis = GizmoAxis::None;
		m_axisDir = glm::vec3{0.0f};
		return true;
	}

	const glm::vec3 dir = GizmoAxisDirection(m_mode, axis, m_before.rotation);
	if (glm::length(dir) < kEpsilon)
		return false;  // unreachable for a finite Euler triple, but never normalize blind

	const glm::vec3 unit = glm::normalize(dir);

	if (m_mode == GizmoMode::Move)
	{
		const ScreenRay ray = vp.RayThrough(cursorPx);
		float t = 0.0f;
		if (!ray.valid || !ClosestParamOnAxis(m_before.position, unit, ray, t))
			return false;  // refused: the axis is within ~1.8 deg of the view ray

		m_grabAnchorT = t;
		m_lastT = t;
	}
	else
	{
		// Rotate and Scale are both screen-space gestures about the projected pivot,
		// so neither is defined while the pivot sits behind the eye.
		const ScreenPoint pivot = vp.Project(m_before.position);
		if (!pivot.visible)
			return false;

		if (m_mode == GizmoMode::Rotate)
		{
			const glm::vec2 d = cursorPx - pivot.pixel;
			m_prevAngle = std::atan2(d.y, d.x);
			m_accumAngle = 0.0f;
			m_rotSign = LatchRotationSign(unit, m_before.position, vp);
			m_rotLatched = true;
		}
		else
		{
			m_anchorPx = cursorPx;
		}
	}

	m_axis = axis;
	m_axisDir = unit;
	return true;
}

void TransformGizmo::Cancel(Transform3D& target)
{
	if (m_mode == GizmoMode::None)
		return;

	// Component-wise and only when different: every setter eagerly rebuilds the
	// model matrix, and untouched components are always unchanged.
	if (target.GetPosition() != m_before.position)
		target.SetPosition(m_before.position);
	if (target.GetRotation() != m_before.rotation)
		target.SetRotation(m_before.rotation);
	if (target.GetScale() != m_before.scale)
		target.SetScale(m_before.scale);
}

void TransformGizmo::Disarm()
{
	m_mode = GizmoMode::None;
	m_axis = GizmoAxis::None;
	m_objectUid = 0;
	m_axisDir = glm::vec3{0.0f};
}

bool TransformGizmo::Drag(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target)
{
	if (m_mode == GizmoMode::None || !vp.IsValid())
		return false;

	if (m_axis == GizmoAxis::None)
	{
		// All three bare gestures are live. Rotate's free variant cannot be
		// `before + theta` on one stored Euler component, so it composes and
		// decomposes a matrix instead (see DragRotateFree).
		switch (m_mode)
		{
		case GizmoMode::Move:   return DragMoveFree(vp, cursorPx, target);
		case GizmoMode::Scale:  return DragScaleFree(vp, cursorPx, target);
		case GizmoMode::Rotate: return DragRotateFree(vp, cursorPx, target);
		default:                return false;
		}
	}

	switch (m_mode)
	{
	case GizmoMode::Move:   return DragMove(vp, cursorPx, target);
	case GizmoMode::Rotate: return DragRotate(vp, cursorPx, target);
	case GizmoMode::Scale:  return DragScale(vp, cursorPx, target);
	default:                return false;
	}
}

bool TransformGizmo::DragMove(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target)
{
	const ScreenRay ray = vp.RayThrough(cursorPx);
	if (!ray.valid)
		return false;

	// Freeze on the last valid parameter rather than clamping: the camera can orbit
	// mid-gesture until the axis is edge-on, and a clamp there jumps by metres.
	float t = m_lastT;
	if (ClosestParamOnAxis(m_before.position, m_axisDir, ray, t))
		m_lastT = t;
	else
		t = m_lastT;

	const glm::vec3 next = m_before.position + (t - m_grabAnchorT) * m_axisDir;
	if (next == target.GetPosition())
		return false;

	target.SetPosition(next);
	return true;
}

bool TransformGizmo::DragRotate(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target)
{
	const ScreenPoint pivot = vp.Project(m_before.position);
	if (!pivot.visible)
		return false;

	const glm::vec2 d = cursorPx - pivot.pixel;
	if (glm::length(d) < kMinRadiusPx)
		return false;  // the bearing is noise this close in; hold the accumulator

	// Unwrapped accumulation, so past 180 degrees and multiple turns both work.
	const float bearing = std::atan2(d.y, d.x);
	m_accumAngle += std::remainder(bearing - m_prevAngle, kTwoPi);
	m_prevAngle = bearing;

	// Exactly `before + theta` on one stored component: no quaternion and no matrix
	// decomposition, so the stored number stays the one the user sees.
	glm::vec3 next = m_before.rotation;
	next[ComponentIndex(m_axis)] += m_rotSign * glm::degrees(m_accumAngle);
	if (next == target.GetRotation())
		return false;

	target.SetRotation(next);
	return true;
}

bool TransformGizmo::DragScale(const EditorViewport& vp, glm::vec2 cursorPx, Transform3D& target)
{
	const ScreenPoint pivot = vp.Project(m_before.position);
	if (!pivot.visible)
		return false;

	// Both radii re-measured against the *current* pivot pixel, and both clamped the
	// same way, so the factor is exactly 1 at the anchor however the camera moved.
	const float current = std::max(glm::length(cursorPx - pivot.pixel), kMinRadiusPx);
	const float anchor = std::max(glm::length(m_anchorPx - pivot.pixel), kMinRadiusPx);
	const float factor = current / anchor;

	// A length ratio, so a mirrored (negative) component keeps its sign.
	glm::vec3 next = m_before.scale;
	const int k = ComponentIndex(m_axis);
	next[k] = m_before.scale[k] * factor;
	if (std::abs(next[k]) < kMinScaleComponent)
		next[k] = std::copysign(kMinScaleComponent, next[k]);

	if (next == target.GetScale())
		return false;

	target.SetScale(next);
	return true;
}

bool TransformGizmo::DragMoveFree(const EditorViewport& vp, glm::vec2 cursorPx,
                                  Transform3D& target)
{
	// Both ends solved on the same plane in the same frame, so the two calls share
	// every camera term: the difference is exactly zero at the anchor pixel (no
	// first-frame snap) and an orbit mid-gesture re-derives both ends. Either call
	// failing leaves the object where it is.
	glm::vec3 from{0.0f};
	glm::vec3 to{0.0f};
	if (!ViewPlanePoint(vp, m_anchorPx, m_before.position, from) ||
	    !ViewPlanePoint(vp, cursorPx, m_before.position, to))
		return false;

	const glm::vec3 next = m_before.position + (to - from);
	if (next == target.GetPosition())
		return false;

	target.SetPosition(next);
	return true;
}

bool TransformGizmo::DragScaleFree(const EditorViewport& vp, glm::vec2 cursorPx,
                                   Transform3D& target)
{
	const ScreenPoint pivot = vp.Project(m_before.position);
	if (!pivot.visible)
		return false;

	// DragScale's radius ratio verbatim: same current pivot pixel, same clamp on
	// both radii, so the factor is exactly 1 at the anchor however the camera moved.
	const float current = std::max(glm::length(cursorPx - pivot.pixel), kMinRadiusPx);
	const float anchor = std::max(glm::length(m_anchorPx - pivot.pixel), kMinRadiusPx);
	const float factor = current / anchor;

	// Uniform: one factor on all three components, which preserves the shape and,
	// being a length ratio, keeps a mirrored component's sign.
	glm::vec3 next = m_before.scale * factor;
	for (int k = 0; k < 3; ++k)
	{
		if (std::abs(next[k]) < kMinScaleComponent)
			next[k] = std::copysign(kMinScaleComponent, next[k]);
	}

	if (next == target.GetScale())
		return false;

	target.SetScale(next);
	return true;
}

bool TransformGizmo::DragRotateFree(const EditorViewport& vp, glm::vec2 cursorPx,
                                    Transform3D& target)
{
	// The axis is the view direction itself, so the object turns in the screen plane and
	// the cursor's bearing about the pivot *is* the angle. Re-read every drag rather than
	// latched, so orbiting mid-gesture keeps turning about what the user is looking down.
	const glm::vec3 viewAxis = vp.ViewAxis();
	if (glm::length(viewAxis) < kEpsilon)
		return false;

	const ScreenPoint pivot = vp.Project(m_before.position);
	if (!pivot.visible)
		return false;

	const glm::vec2 d = cursorPx - pivot.pixel;
	if (glm::length(d) < kMinRadiusPx)
		return false;  // the bearing is noise this close in; hold the accumulator

	// Latched on the first usable drag, not in Arm(): projecting the pivot needs a
	// viewport, and Arm() deliberately takes none. Anchor far enough out is the zero
	// bearing; otherwise this frame's cursor -- a small dead zone instead of an
	// opening jump of up to half a turn.
	if (!m_rotLatched)
	{
		const glm::vec2 anchor = m_anchorPx - pivot.pixel;
		m_prevAngle = (glm::length(anchor) >= kMinRadiusPx) ? std::atan2(anchor.y, anchor.x)
		                                                    : std::atan2(d.y, d.x);
		m_accumAngle = 0.0f;
		// The view axis always points away from the eye, so this always latches +1;
		// going through the same function as the constrained path keeps both gestures'
		// screen-to-world sense identical by construction.
		m_rotSign = LatchRotationSign(viewAxis, m_before.position, vp);
		m_rotLatched = true;
	}

	// Unwrapped accumulation, exactly as DragRotate: past 180 degrees and multiple
	// turns both work, and a drag that returns to its start returns to zero.
	const float bearing = std::atan2(d.y, d.x);
	m_accumAngle += std::remainder(bearing - m_prevAngle, kTwoPi);
	m_prevAngle = bearing;

	// Zero must be *written*, not returned from early: composing at no angle would
	// canonicalise the triple, so an unturned gesture would rewrite equivalent values
	// and record an op for nothing. Writing the snapshot back is also what normalises
	// an axis dropped mid-gesture (W).
	if (m_accumAngle == 0.0f)
	{
		if (m_before.rotation == target.GetRotation())
			return false;

		target.SetRotation(m_before.rotation);
		return true;
	}

	// The decomposition exception, derived from m_before every drag like every other
	// gesture so it stays drift-free; the angle is the accumulator, never an increment.
	const glm::mat4 turn = glm::rotate(glm::mat4{1.0f}, m_rotSign * m_accumAngle, viewAxis);
	const glm::vec3 next =
	    GizmoEulerFromRotation(glm::mat3{turn} * GizmoRotationMatrix(m_before.rotation));

	if (next == target.GetRotation())
		return false;

	target.SetRotation(next);
	return true;
}

} // namespace neurus
