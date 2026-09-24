/**
 * @file GizmoDrawBuilder.cpp
 * @brief Implementation of the resting handle and the modal gizmo's guide geometry.
 *
 * Everything here is sized as a fixed *pixel* budget divided by
 * PixelsPerWorldUnit() at the pivot's depth, so the guide holds its apparent size
 * as the camera dollies. That is also why a camera change has to mark this dirty.
 *
 * Each mode decorates the ends of its guide differently, so the picture says which
 * gesture is live before anything has moved: Move gets an arrowhead, Scale a box
 * handle, Rotate neither — its arc already carries the meaning. Both ends are
 * decorated, because a modal constraint is a bidirectional line. The resting handle
 * is drawn as Move with no axis chosen, so it inherits the arrowheads for free.
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
constexpr float kGuideHalfPx = 110.0f;

/// @brief Radius of the Rotate arc, in logical pixels. Kept inside the guide's
/// half-length so the ring reads as bounded by the axis rather than crossing it.
constexpr float kArcRadiusPx = 85.0f;

/// @brief Chord count for the Rotate arc. 48 keeps the polygon invisible at
/// kArcRadiusPx while staying well inside any sane draw budget.
constexpr int kArcChords = 48;

constexpr float kActiveWidthPx = 4.5f;     ///< The chosen axis.
constexpr float kCandidateWidthPx = 3.0f;  ///< An axis still on offer.
constexpr float kPivotSizePx = 14.0f;      ///< The pivot dot's diameter.

/// @brief How much an unchosen axis' colour is darkened.
///
/// Dimming happens in RGB, never in alpha: a translucent guide washes out over
/// bright geometry and was the first thing to look wrong on screen. The only alpha
/// the gizmo uses is the analytic edge fade OverlayFlag::Smooth applies, which is
/// antialiasing rather than transparency.
constexpr float kCandidateDim = 0.45f;

/// @brief The Move arrowhead: how far back from the tip a barb reaches, and how far
/// out from the axis. 15:7 is a ~25 degree half-angle, which reads as an arrow
/// without the barbs colliding with the line's own width.
constexpr float kArrowBackPx = 15.0f;
constexpr float kArrowRadiusPx = 7.0f;

/// @brief The Scale mode's end handle, in logical pixels. A box, per Blender.
constexpr float kScaleHandlePx = 11.0f;

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

/// @brief Full brightness for the chosen axis, darkened RGB for one still on offer.
/// Alpha is always 1 — see kCandidateDim.
uint32_t GuideColor(GizmoAxis axis, bool active)
{
	const glm::vec3 rgb = AxisColor(axis) * (active ? 1.0f : kCandidateDim);
	return PackOverlayColor(glm::vec4{rgb, 1.0f});
}

/// @brief One overlay segment. The single push site the line, the barbs and the arc
/// all go through, so the AA flag is decided in exactly one place.
void AppendSegment(GizmoDrawList& list, const glm::vec3& a, const glm::vec3& b,
                   float width, uint32_t rgba)
{
	OverlaySegment seg{};
	seg.a = a;
	seg.b = b;
	seg.width = width;  // always pixels: segments ignore ScreenSpaceSize
	seg.rgba = rgba;
	seg.flags = OverlayFlag::Smooth;
	list.segments.push_back(seg);
}

/// @brief The Move arrowhead: four barbs running back from @p tip along -@p dir.
///
/// Four barbs in two perpendicular planes, not a flat V — a V vanishes when its
/// plane turns edge-on to the camera, which for an axis-aligned guide happens at
/// exactly the viewpoints a user orbits to. Not a filled cone either: the overlay
/// payload has no triangle primitive.
void AppendArrowHead(GizmoDrawList& list, const glm::vec3& tip, const glm::vec3& dir,
                     float back, float radius, float width, uint32_t rgba)
{
	glm::vec3 u{0.0f};
	glm::vec3 v{0.0f};
	OrthonormalBasis(dir, u, v);

	const glm::vec3 base = tip - dir * back;
	for (const glm::vec3& out : {u, -u, v, -v})
		AppendSegment(list, tip, base + out * radius, width, rgba);
}

/// @brief The Scale mode's end handle: a box, per Blender. A sprite rather than 12
/// segments because it needs no orientation — it marks a length, not a direction.
void AppendScaleHandle(GizmoDrawList& list, const glm::vec3& at, uint32_t rgba)
{
	OverlayPointSprite box{};
	box.p = at;
	box.size = kScaleHandlePx;
	box.rgba = rgba;
	box.shape = OverlayPointShape::Square;
	box.flags = OverlayFlag::Smooth | OverlayFlag::ScreenSpaceSize;
	list.points.push_back(box);
}

/// @brief One constraint guide: a line through @p pivot centred on it, plus whatever
/// end decoration the mode calls for.
///
/// Centred rather than starting at the pivot because a modal constraint is a
/// bidirectional line, not a grabbable arrow pointing one way — which is also why
/// *both* ends are decorated.
void AppendGuide(GizmoDrawList& list, GizmoMode mode, GizmoAxis axis, bool active,
                 const glm::vec3& pivot, const glm::vec3& dir, float halfLength,
                 float pxPerUnit)
{
	const uint32_t rgba = GuideColor(axis, active);
	const float width = active ? kActiveWidthPx : kCandidateWidthPx;

	const glm::vec3 tipPos = pivot + dir * halfLength;
	const glm::vec3 tipNeg = pivot - dir * halfLength;
	AppendSegment(list, tipNeg, tipPos, width, rgba);

	if (mode == GizmoMode::Move)
	{
		const float back = kArrowBackPx / pxPerUnit;
		const float radius = kArrowRadiusPx / pxPerUnit;
		AppendArrowHead(list, tipPos, dir, back, radius, width, rgba);
		AppendArrowHead(list, tipNeg, -dir, back, radius, width, rgba);
	}
	else if (mode == GizmoMode::Scale)
	{
		AppendScaleHandle(list, tipPos, rgba);
		AppendScaleHandle(list, tipNeg, rgba);
	}
	// Rotate gets neither: the arc is what carries the meaning, and a tip would
	// suggest a direction a rotation axis does not have.
}

/// @brief The Rotate arc: a closed ring of chords in the plane perpendicular to @p dir.
/// It shows the plane the rotation happens in, which a single line cannot.
void AppendArc(GizmoDrawList& list, const glm::vec3& pivot, const glm::vec3& dir,
               float radius, GizmoAxis axis)
{
	glm::vec3 u{0.0f};
	glm::vec3 v{0.0f};
	OrthonormalBasis(dir, u, v);

	const uint32_t rgba = GuideColor(axis, true);

	glm::vec3 prev = pivot + u * radius;
	for (int i = 1; i <= kArcChords; ++i)
	{
		const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(kArcChords);
		const glm::vec3 next = pivot + (u * std::cos(a) + v * std::sin(a)) * radius;
		AppendSegment(list, prev, next, kActiveWidthPx, rgba);
		prev = next;
	}
}

/// @brief The pivot dot. Pushed last of everything, see Rebuild().
void AppendPivot(GizmoDrawList& list, const glm::vec3& pivot)
{
	OverlayPointSprite dot{};
	dot.p = pivot;
	dot.size = kPivotSizePx;
	dot.rgba = PackOverlayColor(glm::vec4{1.0f, 1.0f, 1.0f, 1.0f});
	dot.shape = OverlayPointShape::Circle;
	// The shape mask is an analytic distance, so a circle is visibly stepped without
	// Smooth; ScreenSpaceSize is what makes `size` mean pixels rather than world units.
	dot.flags = OverlayFlag::Smooth | OverlayFlag::ScreenSpaceSize;
	list.points.push_back(dot);
}

} // namespace

void GizmoDrawBuilder::Rebuild(const TransformGizmo& gizmo, const EditorViewport& vp,
                               const TransformSnapshot* resting)
{
	if (!m_dirty)
		return;

	m_dirty = false;
	m_list.Clear();

	if (!vp.IsValid())
		return;

	// An armed gesture always wins over the resting handle, and it anchors on its own
	// frozen Before() rather than on the object: mid-drag the object has already moved,
	// so anchoring there would drag the guide along with it and destroy the reference
	// the user is measuring against. With nothing armed and nothing selected there is
	// no anchor at all and the list stays empty.
	const bool armed = gizmo.IsActive();
	if (!armed && !resting)
		return;

	const TransformSnapshot& anchor = armed ? gizmo.Before() : *resting;

	// Resting is Move, unconstrained — the same state G leaves the gesture in, which
	// is why the whole picture below is shared rather than special-cased.
	const GizmoMode mode = armed ? gizmo.Mode() : GizmoMode::Move;
	const GizmoAxis axis = armed ? gizmo.Axis() : GizmoAxis::None;

	// Constant for a whole gesture (Rotate and Scale do not move the object, Move's
	// line is anchored at the start), so projecting it once sizes everything.
	const glm::vec3 pivot = anchor.position;
	const ScreenPoint screen = vp.Project(pivot);
	if (!screen.visible)
		return;

	const float pxPerUnit = PixelsPerWorldUnit(vp, screen.viewDepth);
	if (pxPerUnit <= 0.0f)
		return;

	const float halfLength = kGuideHalfPx / pxPerUnit;

	if (axis == GizmoAxis::None)
	{
		// Two states share this branch and brightness is what separates them. Resting
		// draws full-strength: nothing is "unchosen" yet, so there is no contrast for
		// dimming to carry, and this is the affordance that says where the selection is
		// and which way its local axes point. Armed-but-unconstrained dims instead —
		// against the axis about to light up, the dim reads as "still on offer", and the
		// dip in brightness on pressing G is itself the feedback that a gesture started.
		for (const GizmoAxis candidate : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z})
		{
			const glm::vec3 dir = GizmoAxisDirection(mode, candidate, anchor.rotation);
			if (glm::length(dir) > 0.0f)
				AppendGuide(m_list, mode, candidate, !armed, pivot, dir, halfLength, pxPerUnit);
		}
	}
	else
	{
		const glm::vec3 dir = GizmoAxisDirection(mode, axis, anchor.rotation);
		if (glm::length(dir) > 0.0f)
		{
			AppendGuide(m_list, mode, axis, true, pivot, dir, halfLength, pxPerUnit);
			if (mode == GizmoMode::Rotate)
				AppendArc(m_list, pivot, dir, kArcRadiusPx / pxPerUnit, axis);
		}
	}

	// Last of everything: depth testing is off and blending is alpha-over, so draw
	// order is what decides what lands on top (GizmoDrawList.h records the contract).
	AppendPivot(m_list, pivot);
}

} // namespace neurus
