/**
 * @file GizmoDrawBuilder.cpp
 * @brief Implementation of the modal gizmo's guide geometry.
 *
 * Everything here is sized as a fixed *pixel* budget divided by
 * PixelsPerWorldUnit() at the pivot's depth, so the guide holds its apparent size
 * as the camera dollies. That is also why a camera change has to mark this dirty.
 */

#include "editor/viewport/GizmoDrawBuilder.h"

#include <cmath>

#include <glm/glm.hpp>

#include "editor/viewport/EditorViewport.h"
#include "editor/viewport/TransformGizmo.h"

namespace neurus
{

namespace
{

/// @brief Half-length of a constraint guide, in logical pixels.
constexpr float kGuideHalfPx = 220.0f;

/// @brief Radius of the Rotate arc, in logical pixels.
constexpr float kArcRadiusPx = 90.0f;

/// @brief Chord count for the Rotate arc. 48 keeps the polygon invisible at
/// kArcRadiusPx while staying well inside any sane draw budget.
constexpr int kArcChords = 48;

constexpr float kActiveWidthPx = 2.5f;     ///< The chosen axis.
constexpr float kCandidateWidthPx = 1.5f;  ///< An axis still on offer.
constexpr float kPivotSizePx = 7.0f;       ///< The pivot dot's diameter.

constexpr float kActiveAlpha = 1.0f;
constexpr float kCandidateAlpha = 0.45f;

constexpr float kTwoPi = 6.283185307179586f;

/// @brief Blender's axis convention: X red, Y green, Z blue.
glm::vec3 AxisColor(GizmoAxis axis)
{
	switch (axis)
	{
	case GizmoAxis::X: return glm::vec3{1.0f, 0.24f, 0.33f};
	case GizmoAxis::Y: return glm::vec3{0.47f, 0.82f, 0.18f};
	case GizmoAxis::Z: return glm::vec3{0.22f, 0.51f, 1.0f};
	default: return glm::vec3{1.0f};
	}
}

/// @brief Any two unit vectors perpendicular to @p n and to each other.
/// Seeded from whichever cardinal axis @p n is least aligned with, so the cross
/// product is never near-degenerate.
void OrthonormalBasis(const glm::vec3& n, glm::vec3& outU, glm::vec3& outV)
{
	const glm::vec3 seed = (std::abs(n.z) < 0.9f) ? glm::vec3{0.0f, 0.0f, 1.0f}
	                                              : glm::vec3{1.0f, 0.0f, 0.0f};
	outU = glm::normalize(glm::cross(seed, n));
	outV = glm::cross(n, outU);
}

/// @brief One constraint guide: a line through @p pivot, centred on it.
/// Centred rather than starting at the pivot because a modal constraint is a
/// bidirectional line, not a grabbable arrow pointing one way.
void AppendAxisLine(GizmoDrawList& list, const glm::vec3& pivot, const glm::vec3& dir,
                    float halfLength, GizmoAxis axis, bool active)
{
	OverlaySegment seg{};
	seg.a = pivot - dir * halfLength;
	seg.b = pivot + dir * halfLength;
	seg.width = active ? kActiveWidthPx : kCandidateWidthPx;
	seg.rgba = PackOverlayColor(glm::vec4{AxisColor(axis), active ? kActiveAlpha : kCandidateAlpha});
	seg.flags = OverlayFlag::Smooth;  // a solid line, so analytic edge AA applies
	list.segments.push_back(seg);
}

/// @brief The Rotate arc: a closed ring of chords in the plane perpendicular to @p dir.
/// It shows the plane the rotation happens in, which a single line cannot.
void AppendArc(GizmoDrawList& list, const glm::vec3& pivot, const glm::vec3& dir,
               float radius, GizmoAxis axis)
{
	glm::vec3 u{0.0f};
	glm::vec3 v{0.0f};
	OrthonormalBasis(dir, u, v);

	const uint32_t rgba = PackOverlayColor(glm::vec4{AxisColor(axis), kActiveAlpha});

	glm::vec3 prev = pivot + u * radius;
	for (int i = 1; i <= kArcChords; ++i)
	{
		const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(kArcChords);
		const glm::vec3 next = pivot + (u * std::cos(a) + v * std::sin(a)) * radius;

		OverlaySegment seg{};
		seg.a = prev;
		seg.b = next;
		seg.width = kActiveWidthPx;
		seg.rgba = rgba;
		seg.flags = OverlayFlag::Smooth;
		list.segments.push_back(seg);

		prev = next;
	}
}

/// @brief The pivot dot. Pushed last of everything, see Rebuild().
void AppendPivot(GizmoDrawList& list, const glm::vec3& pivot)
{
	OverlayPointSprite dot{};
	dot.p = pivot;
	dot.size = kPivotSizePx;
	dot.rgba = PackOverlayColor(glm::vec4{1.0f, 1.0f, 1.0f, 0.95f});
	dot.shape = OverlayPointShape::Circle;
	// The shape mask is an analytic distance, so a circle is visibly stepped without
	// Smooth; ScreenSpaceSize is what makes `size` mean pixels rather than world units.
	dot.flags = OverlayFlag::Smooth | OverlayFlag::ScreenSpaceSize;
	list.points.push_back(dot);
}

} // namespace

void GizmoDrawBuilder::Rebuild(const TransformGizmo& gizmo, const EditorViewport& vp)
{
	if (!m_dirty)
		return;

	m_dirty = false;
	m_list.Clear();

	if (!gizmo.IsActive() || !vp.IsValid())
		return;

	// The pivot is the before-position for all three modes and constant for the whole
	// gesture: Rotate and Scale do not move the object, and Move's line is anchored
	// there. Projecting it once is therefore enough to size everything.
	const glm::vec3 pivot = gizmo.Before().position;
	const ScreenPoint screen = vp.Project(pivot);
	if (!screen.visible)
		return;

	const float pxPerUnit = PixelsPerWorldUnit(vp, screen.viewDepth);
	if (pxPerUnit <= 0.0f)
		return;

	const GizmoMode mode = gizmo.Mode();
	const GizmoAxis axis = gizmo.Axis();
	const float halfLength = kGuideHalfPx / pxPerUnit;

	if (axis == GizmoAxis::None)
	{
		// Armed but unconstrained: offer all three, dimmed. Nothing is transformed yet
		// — Wave 1 has no view-plane gesture, so this state is purely a prompt.
		for (const GizmoAxis candidate : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z})
		{
			const glm::vec3 dir = GizmoAxisDirection(mode, candidate, gizmo.Before().rotation);
			if (glm::length(dir) > 0.0f)
				AppendAxisLine(m_list, pivot, dir, halfLength, candidate, false);
		}
	}
	else
	{
		const glm::vec3 dir = GizmoAxisDirection(mode, axis, gizmo.Before().rotation);
		if (glm::length(dir) > 0.0f)
		{
			AppendAxisLine(m_list, pivot, dir, halfLength, axis, true);
			if (mode == GizmoMode::Rotate)
				AppendArc(m_list, pivot, dir, kArcRadiusPx / pxPerUnit, axis);
		}
	}

	// Last of everything: depth testing is off and blending is alpha-over, so draw
	// order is what decides what lands on top (GizmoDrawList.h records the contract).
	AppendPivot(m_list, pivot);
}

} // namespace neurus
