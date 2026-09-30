/**
 * @file TransformGizmo.cpp
 * @brief Implementation of the modal G / R / S state machine.
 *
 * Three things here are load-bearing and each is easy to "tidy" into a bug:
 *
 *  1. Every drag derives the new value from m_before, never from the transform's
 *     current value. That is the whole drift-free property, and it is also what
 *     makes a mid-gesture axis switch snap cleanly back to the before-state.
 *  2. The Rotate sign is read once, in Constrain(). Re-deriving `facing` per frame
 *     flips it chaotically whenever the axis passes edge-on.
 *  3. Degenerate Move freezes on the last valid parameter instead of clamping to a
 *     bound. Clamping would teleport the object by metres on a single pixel.
 *  4. The two free gestures measure from m_anchorPx — the pixel the mode key was
 *     pressed at — and solve *both* ends against the camera every drag. That is what
 *     makes the delta exactly zero at the anchor and what lets the camera orbit
 *     mid-gesture without the object jumping.
 *  5. The free Rotate is the one gesture that leaves the `before + theta` identity:
 *     the view axis is not a one-parameter subgroup of the stored Euler triple, so it
 *     composes a matrix and reads the triple back. Still derived from m_before every
 *     drag, so property 1 is untouched.
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

/// @brief Below this, 1 - dot(axis, ray)^2 makes the closest-point solve meaningless.
/// 1e-3 is an axis within ~1.8 degrees of the view ray.
constexpr float kMinAxisSeparation = 1e-3f;

/// @brief Edge-on band for the Rotate sign latch, and the zero-length vector guard.
constexpr float kEpsilon = 1e-3f;

/// @brief Screen radius floor for Rotate/Scale, in logical pixels.
/// Clamping both the anchor and the cursor radius is what makes the scale factor
/// exactly 1 at the anchor, so there is no first-frame snap.
constexpr float kMinRadiusPx = 8.0f;

/// @brief Smallest |scale| component we will write.
/// Transform::GetNormalMatrix() inverts mat3(model); a zero component makes it
/// singular, so a collapsing drag is pinned just short of zero instead.
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
/// The standard line-line closest-point solve with both directions unit length:
/// a = dot(u,u) = 1, c = dot(r,r) = 1, so sc = (b*e - d) / (1 - b*b).
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
	// The exact construction of Transform.cpp's ComputeModelMatrix, minus T and S.
	const glm::vec3 rad = glm::radians(rotationDegrees);
	const glm::mat4 rz = glm::rotate(glm::mat4{1.0f}, rad.z, glm::vec3{0.0f, 0.0f, 1.0f});
	const glm::mat4 rx = glm::rotate(glm::mat4{1.0f}, rad.x, glm::vec3{1.0f, 0.0f, 0.0f});
	const glm::mat4 ry = glm::rotate(glm::mat4{1.0f}, rad.y, glm::vec3{0.0f, 1.0f, 0.0f});
	return glm::mat3{rz * rx * ry};
}

glm::vec3 GizmoEulerFromRotation(const glm::mat3& r)
{
	// Solving Rz(yaw) * Rx(pitch) * Ry(roll) = r. Written out, that product is
	//
	//     [ cc*cb - sc*sa*sb   -sc*ca    cc*sb + sc*sa*cb ]
	//     [ sc*cb + cc*sa*sb    cc*ca    sc*sb - cc*sa*cb ]
	//     [       -ca*sb          sa           ca*cb      ]
	//
	// with a = pitch, b = roll, c = yaw. Math-notation M(row, col) is glm's r[col][row],
	// so every index below is transposed against that picture.
	const float pitch = std::asin(std::clamp(r[1][2], -1.0f, 1.0f));

	// cos(pitch) is non-negative over asin's whole range, so its own size is the only
	// degeneracy test needed -- no quadrant case analysis.
	if (std::cos(pitch) < kEpsilon)
	{
		// Gimbal lock. With sa = +-1 the first column collapses to cos/sin of (c +- b):
		// yaw and roll turn about the same world axis and only that combination is
		// observable. Pinning roll to 0 picks the one representative.
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
		// The gimbal set: the axis each stored Euler component actually turns about.
		// Built from the partial products, which is why this branch does not go through
		// GizmoRotationMatrix.
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
 * viewer runs the cursor bearing backwards. That case takes -1, and so does the
 * edge-on band where `facing` is numerically meaningless — re-deriving this every
 * frame is what would make the drag flip chaotically as the axis crosses zero.
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
 * The plane's normal is EditorViewport::ViewAxis(), so the plane is exactly parallel to
 * the screen: one pixel of cursor motion is one pixel of world motion at the pivot's
 * depth, with no foreshortening and no depth to guess. Anchored on the *before* position
 * rather than on the object, so the plane cannot drift as the drag carries the object off
 * it.
 *
 * ScreenRay::origin sits on the near plane, not at the eye, which is exactly why this
 * is a plane intersection from that origin and not a scale of an eye-relative depth.
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
		// W: back to the screen-space variant. Re-latching every free anchor here is what
		// makes it the mirror image of an axis switch -- the one drag the controller issues
		// next reproduces the before-state, so dropping the constraint *undoes* whatever
		// the axis had applied rather than carrying it over into the free gesture.
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

	// Component-wise and only when different: every setter eagerly rebuilds the model
	// matrix, and the two components a gesture never touches are always unchanged.
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
		// All three bare gestures are live, as in Blender. Rotate's free variant is the
		// one that cannot be `before + theta` on a single stored Euler component, so it
		// composes and decomposes a matrix instead (see DragRotateFree).
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

	// Exactly `before + theta` on one stored component: this is the whole reason the
	// feature needs no quaternion and no matrix decomposition, and it is what keeps
	// the PropertyPanel spinbox showing a number the user can reason about.
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
	// Both ends solved on the same plane, in the same frame, so the two calls share
	// every camera term: the difference is exactly zero at the anchor pixel (no
	// first-frame snap) and an orbit mid-gesture re-derives both ends rather than
	// jumping. Either call failing leaves the object where it is.
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

	// DragScale's radius ratio verbatim — same current pivot pixel, same clamp on both
	// radii, so the factor is exactly 1 at the anchor however the camera moved.
	const float current = std::max(glm::length(cursorPx - pivot.pixel), kMinRadiusPx);
	const float anchor = std::max(glm::length(m_anchorPx - pivot.pixel), kMinRadiusPx);
	const float factor = current / anchor;

	// Uniform: one factor on all three components, which preserves the object's shape
	// and, being a length ratio, keeps a mirrored component's sign.
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
	// viewport, and Arm() deliberately takes none. The anchor pixel is the zero bearing
	// when it is far enough out to have one, and this frame's cursor otherwise -- a small
	// dead zone at the start instead of an opening jump of up to half a turn.
	if (!m_rotLatched)
	{
		const glm::vec2 anchor = m_anchorPx - pivot.pixel;
		m_prevAngle = (glm::length(anchor) >= kMinRadiusPx) ? std::atan2(anchor.y, anchor.x)
		                                                    : std::atan2(d.y, d.x);
		m_accumAngle = 0.0f;
		// The view axis always points away from the eye, so this always latches +1. Going
		// through the same function as the constrained path is what makes the two
		// gestures' screen-to-world sense identical by construction rather than by luck.
		m_rotSign = LatchRotationSign(viewAxis, m_before.position, vp);
		m_rotLatched = true;
	}

	// Unwrapped accumulation, exactly as DragRotate: past 180 degrees and multiple turns
	// both work, and a drag that returns to its start returns to zero.
	const float bearing = std::atan2(d.y, d.x);
	m_accumAngle += std::remainder(bearing - m_prevAngle, kTwoPi);
	m_prevAngle = bearing;

	// Zero means zero, and it has to mean it by *writing* m_before rather than by
	// returning early: composing at no angle would canonicalise the triple, so an
	// unturned gesture would rewrite equivalent-but-different numbers and leave
	// SubmitGesture recording an op for nothing. Writing the snapshot back verbatim is
	// also exactly what W needs -- it is how dropping an axis mid-gesture normalises
	// whatever that axis had already applied.
	if (m_accumAngle == 0.0f)
	{
		if (m_before.rotation == target.GetRotation())
			return false;

		target.SetRotation(m_before.rotation);
		return true;
	}

	// The whole of the decomposition exception, two lines of it. Derived from m_before
	// every drag like every other gesture, so it stays drift-free; the angle is the
	// accumulator, never an increment applied to the value written last frame.
	const glm::mat4 turn = glm::rotate(glm::mat4{1.0f}, m_rotSign * m_accumAngle, viewAxis);
	const glm::vec3 next =
	    GizmoEulerFromRotation(glm::mat3{turn} * GizmoRotationMatrix(m_before.rotation));

	if (next == target.GetRotation())
		return false;

	target.SetRotation(next);
	return true;
}

} // namespace neurus
