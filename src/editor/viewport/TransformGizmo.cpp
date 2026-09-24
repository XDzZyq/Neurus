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

glm::vec3 GizmoAxisDirection(GizmoMode mode, GizmoAxis axis, const glm::vec3& rotationDegrees)
{
	if (mode == GizmoMode::None || axis == GizmoAxis::None)
		return glm::vec3{0.0f};

	// The exact construction of Transform.cpp's ComputeModelMatrix, minus T and S.
	const glm::vec3 rad = glm::radians(rotationDegrees);
	const glm::mat4 rz = glm::rotate(glm::mat4{1.0f}, rad.z, glm::vec3{0.0f, 0.0f, 1.0f});
	const glm::mat4 rx = glm::rotate(glm::mat4{1.0f}, rad.x, glm::vec3{1.0f, 0.0f, 0.0f});
	const glm::mat4 ry = glm::rotate(glm::mat4{1.0f}, rad.y, glm::vec3{0.0f, 1.0f, 0.0f});

	if (mode == GizmoMode::Rotate)
	{
		// The gimbal set: the axis each stored Euler component actually turns about.
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
	const glm::mat4 r = rz * rx * ry;
	switch (axis)
	{
	case GizmoAxis::X: return glm::normalize(glm::vec3{r[0]});
	case GizmoAxis::Y: return glm::normalize(glm::vec3{r[1]});
	case GizmoAxis::Z: return glm::normalize(glm::vec3{r[2]});
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
 * The plane's normal is the camera's own view axis, normalize(cam_tar - eye), so the
 * plane is exactly parallel to the screen: one pixel of cursor motion is one pixel of
 * world motion at the pivot's depth, with no foreshortening and no depth to guess.
 * Anchored on the *before* position rather than on the object, so the plane cannot
 * drift as the drag carries the object off it.
 *
 * ScreenRay::origin sits on the near plane, not at the eye, which is exactly why this
 * is a plane intersection from that origin and not a scale of an eye-relative depth.
 */
bool ViewPlanePoint(const EditorViewport& vp, glm::vec2 cursorPx, const glm::vec3& planePoint,
                    glm::vec3& out)
{
	const Camera* cam = vp.GetCamera();
	if (!cam)
		return false;

	const glm::vec3 toTarget = cam->cam_tar - cam->GetPosition();
	const float len = glm::length(toTarget);
	if (len < kEpsilon)
		return false;  // a camera looking at its own eye has no view axis

	const ScreenRay ray = vp.RayThrough(cursorPx);
	if (!ray.valid)
		return false;

	const glm::vec3 n = toTarget / len;
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

	// The free gesture's only anchor. A key press carries no cursor of its own, so the
	// caller hands in the retained one; Constrain() re-latches it for Scale.
	m_anchorPx = cursorPx;
	return true;
}

bool TransformGizmo::Constrain(GizmoAxis axis, const EditorViewport& vp, glm::vec2 cursorPx)
{
	if (m_mode == GizmoMode::None || axis == GizmoAxis::None || !vp.IsValid())
		return false;

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
		// Bare G and bare S are live gestures, as in Blender. Bare R is not: a rotation
		// about the view axis is not `before + theta` on one stored Euler component, and
		// that identity is what lets this whole feature avoid quaternions and matrix
		// decomposition (see GizmoAxisDirection). R therefore waits for an axis key.
		switch (m_mode)
		{
		case GizmoMode::Move:  return DragMoveFree(vp, cursorPx, target);
		case GizmoMode::Scale: return DragScaleFree(vp, cursorPx, target);
		default:               return false;
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

} // namespace neurus
