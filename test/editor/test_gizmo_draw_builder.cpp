/**
 * @file test_gizmo_draw_builder.cpp
 * @brief CPU verification of GizmoDrawBuilder: what the guide looks like, and when.
 *
 * The builder's output is a vertex buffer. On screen a wrong list still draws
 * *something* plausible — lines at a pivot, in axis colours — so five contracts
 * nothing downstream can check:
 *
 *   1. The list is derived, never authored. One Rebuild() per dirty flag, and a
 *      clean builder ignores every change made since. Everything that changes the
 *      picture has to go through MarkDirty(), which is why the gate is pinned first:
 *      a stale list is indistinguishable from a correct one in a screenshot.
 *   2. The primitive counts *are* the mode. Move ends in arrowheads, Scale in boxes,
 *      Rotate in neither plus a ring, and a free Rotate in four rings. A mode that
 *      quietly drew another mode's decoration looks right in a screenshot of the
 *      other mode.
 *   3. The pivot rides the object; the rotation does not. Only a Rotate can tell the
 *      two sources apart, and only if the guide's own direction is checked against
 *      the frozen snapshot — guides that spun with the object would destroy the
 *      reference the rotation is measured against.
 *   4. Every size is a fixed pixel budget over PixelsPerWorldUnit(), so the world
 *      length must scale with depth. That is the whole reason a camera dolly is a
 *      rebuild.
 *   5. Dimming is RGB, never alpha. A translucent guide washes out over bright
 *      geometry, so every primitive is fully opaque in every state.
 *
 * No GPU: a bare Camera, an EditorViewport and a Transform3D, exactly as the Editor
 * pushes them in.
 */

#include <gtest/gtest.h>

#include "editor/viewport/GizmoDrawBuilder.h"
#include "editor/viewport/EditorViewport.h"
#include "editor/viewport/TransformGizmo.h"
#include "scene/Camera.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

using namespace neurus;

namespace {

constexpr uint32_t kW = 800u;
constexpr uint32_t kH = 600u;
constexpr int      kUid = 7;

/// @brief Chords in one Rotate ring. Mirrors GizmoDrawBuilder.cpp's kArcChords: the
/// count is the only handle a test has on "this is a ring and not a line".
constexpr size_t kChords = 48u;

/// @brief Segments in one Move guide: the line plus two arrowheads of four barbs.
constexpr size_t kMoveGuideSegments = 9u;

float SegLength(const OverlaySegment& s)
{
	return glm::length(s.b - s.a);
}

/**
 * @brief The guide's axis line.
 *
 * Found by length rather than by index: the line spans 144 logical pixels while an
 * arrowhead barb is ~17 and an arc chord ~7, so the longest segment is the line by a
 * wide margin — and the answer does not depend on the order things were pushed in.
 */
const OverlaySegment& AxisLine(const GizmoDrawList& list)
{
	return *std::max_element(list.segments.begin(), list.segments.end(),
	                         [](const OverlaySegment& a, const OverlaySegment& b) {
		                         return SegLength(a) < SegLength(b);
	                         });
}

size_t CountSegments(const GizmoDrawList& list, uint32_t rgba)
{
	return static_cast<size_t>(std::count_if(
	    list.segments.begin(), list.segments.end(),
	    [rgba](const OverlaySegment& s) { return s.rgba == rgba; }));
}

size_t CountPoints(const GizmoDrawList& list, uint32_t shape)
{
	return static_cast<size_t>(std::count_if(
	    list.points.begin(), list.points.end(),
	    [shape](const OverlayPointSprite& p) { return p.shape == shape; }));
}

/// @brief Farthest chord endpoint from @p pivot among the segments @p pick accepts.
template <typename Pick>
float RingRadius(const GizmoDrawList& list, const glm::vec3& pivot, Pick pick)
{
	float r = 0.0f;
	for (const OverlaySegment& s : list.segments)
	{
		if (pick(s))
			r = std::max(r, glm::length(s.a - pivot));
	}
	return r;
}

/// @brief Contract 5: nothing the gizmo draws is ever translucent.
bool AllOpaque(const GizmoDrawList& list)
{
	for (const OverlaySegment& s : list.segments)
	{
		if (((s.rgba >> 24) & 0xFFu) != 0xFFu)
			return false;
	}
	for (const OverlayPointSprite& p : list.points)
	{
		if (((p.rgba >> 24) & 0xFFu) != 0xFFu)
			return false;
	}
	return true;
}

/// @brief The colour a free Rotate's view ring is drawn in: plain white, full alpha.
const uint32_t kWhite = PackOverlayColor(glm::vec4(1.0f));

} // namespace

/**
 * @brief The project's camera convention: eye at (0,-5,0), target the origin, Z up.
 *
 * Same pose as test_editor_viewport.cpp, for the same reason — world +Y is exactly
 * the view axis, so "the ring a free Rotate turns about" is a nameable direction.
 */
class GizmoDrawBuilderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_cam.ChangeCamRatio(static_cast<float>(kW), static_cast<float>(kH));
		m_cam.SetPosition(glm::vec3(0.0f, -5.0f, 0.0f));
		m_cam.SetTarPos(glm::vec3(0.0f));

		m_vp.SetViewportSize(glm::uvec2(kW, kH), glm::uvec2(kW, kH));
		m_vp.SetCamera(&m_cam);
	}

	/// @brief A live anchor at @p position: no rotation, unit scale.
	static TransformSnapshot Anchor(const glm::vec3& position = glm::vec3(0.0f))
	{
		TransformSnapshot snap{};
		snap.position = position;
		return snap;
	}

	/// @brief The pivot dot, which Rebuild() pushes last of everything.
	const OverlayPointSprite& Pivot() const { return m_builder.List().points.back(); }

	Camera           m_cam;
	EditorViewport   m_vp;
	TransformGizmo   m_gizmo;
	GizmoDrawBuilder m_builder;
	Transform3D      m_target;
};

// ===========================================================================
// A. The dirty gate — contract 1
// ===========================================================================

/// @test A fresh builder is dirty, so the very first Edit() populates the list.
TEST_F(GizmoDrawBuilderTest, StartsDirtySoTheFirstEditPopulatesTheList)
{
	EXPECT_TRUE(m_builder.IsDirty());
	EXPECT_TRUE(m_builder.List().Empty());

	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);
	EXPECT_FALSE(m_builder.IsDirty());
	EXPECT_FALSE(m_builder.List().Empty());
}

/**
 * @test A clean builder ignores everything, and MarkDirty is what lets it through.
 *
 * The half of contract 1 that keeps every other test in this file honest: if Rebuild
 * were to run unconditionally, none of them would be testing the dirty flag at all,
 * and in the app a missing MarkDirty would be invisible rather than a frozen guide.
 */
TEST_F(GizmoDrawBuilderTest, ACleanBuilderIgnoresEveryChangeUntilMarkedDirty)
{
	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);
	const size_t resting = m_builder.List().segments.size();
	ASSERT_GT(resting, 0u);

	// The selection is gone: the list would be empty if this rebuild ran.
	m_builder.Rebuild(m_gizmo, m_vp, nullptr);
	EXPECT_EQ(m_builder.List().segments.size(), resting) << "a clean builder is a no-op";

	m_builder.MarkDirty();
	m_builder.Rebuild(m_gizmo, m_vp, nullptr);
	EXPECT_TRUE(m_builder.List().Empty());
}

// ===========================================================================
// B. Nothing to draw is an empty list, never a stale one
// ===========================================================================

/// @test Nothing armed and nothing selected: the list clears rather than persisting.
TEST_F(GizmoDrawBuilderTest, NothingArmedAndNoAnchorIsAnEmptyList)
{
	m_builder.Rebuild(m_gizmo, m_vp, nullptr);
	EXPECT_TRUE(m_builder.List().Empty());
}

/**
 * @test An invalid viewport clears the list instead of leaving the last frame's.
 *
 * This is the state between a scene swap and the first resize, and the guide it would
 * otherwise keep belongs to the previous document.
 */
TEST_F(GizmoDrawBuilderTest, AnInvalidViewportIsAnEmptyList)
{
	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);
	ASSERT_FALSE(m_builder.List().Empty());

	EditorViewport bare;  // no camera, no size
	m_builder.MarkDirty();
	m_builder.Rebuild(m_gizmo, bare, &live);
	EXPECT_TRUE(m_builder.List().Empty());
}

/// @test A pivot behind the eye draws nothing: there is no pixel to size against.
TEST_F(GizmoDrawBuilderTest, APivotBehindTheEyeDrawsNothing)
{
	const TransformSnapshot live = Anchor(glm::vec3(0.0f, -20.0f, 0.0f));
	m_builder.Rebuild(m_gizmo, m_vp, &live);
	EXPECT_TRUE(m_builder.List().Empty());
}

// ===========================================================================
// C. The resting handle — Move with no axis, and nothing else
// ===========================================================================

/**
 * @test A selection with no gesture draws three Move guides and one pivot dot.
 *
 * The count is the assertion: three guides of nine segments and not one more. A ring
 * would add 48 at a stroke, which is what makes this the check that the resting handle
 * has not picked up a Rotate's decoration.
 */
TEST_F(GizmoDrawBuilderTest, RestingHandleIsThreeMoveGuidesAndOnePivotDot)
{
	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);

	EXPECT_EQ(m_builder.List().segments.size(), 3u * kMoveGuideSegments);
	EXPECT_EQ(m_builder.List().points.size(), 1u) << "the pivot dot, and no box handles";
	EXPECT_EQ(CountPoints(m_builder.List(), OverlayPointShape::Circle), 1u);
}

/**
 * @test The pivot dot is pushed last and sits on the anchor.
 *
 * Depth testing is off and blending is alpha-over, so draw order is the only thing
 * deciding what lands on top: the dot has to be after every line or the guide crosses
 * over it.
 */
TEST_F(GizmoDrawBuilderTest, PivotDotIsPushedLastAndSitsOnTheAnchor)
{
	const TransformSnapshot live = Anchor(glm::vec3(0.5f, 0.0f, -0.25f));
	m_builder.Rebuild(m_gizmo, m_vp, &live);

	ASSERT_FALSE(m_builder.List().points.empty());
	EXPECT_EQ(Pivot().shape, OverlayPointShape::Circle);
	EXPECT_LT(glm::length(Pivot().p - live.position), 1e-5f);
	EXPECT_TRUE((Pivot().flags & OverlayFlag::ScreenSpaceSize) != 0u)
	    << "the dot is a fixed pixel size, not a world-space sphere";
}

// ===========================================================================
// D. Where the guide is anchored — contract 3
// ===========================================================================

/**
 * @test A gesture whose target was deleted mid-drag falls back to its own snapshot.
 *
 * The object can go away between two drags of one frame, and the guide must not blink
 * out on the last frame of a delete: an armed gesture is its own anchor of last resort.
 */
TEST_F(GizmoDrawBuilderTest, AnArmedGestureFallsBackToItsSnapshotWhenTheTargetIsGone)
{
	m_target.SetPosition(glm::vec3(2.0f, 0.0f, 0.0f));
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, glm::vec2(kW, kH) * 0.5f));

	m_builder.Rebuild(m_gizmo, m_vp, nullptr);
	ASSERT_FALSE(m_builder.List().Empty());
	EXPECT_LT(glm::length(Pivot().p - glm::vec3(2.0f, 0.0f, 0.0f)), 1e-5f);
}

/// @test While a target exists the guide rides its live position, not the snapshot.
TEST_F(GizmoDrawBuilderTest, TheGuideRidesTheLiveAnchorDuringAMove)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, glm::vec2(kW, kH) * 0.5f));

	const TransformSnapshot live = Anchor(glm::vec3(1.0f, 0.0f, 0.0f));
	m_builder.Rebuild(m_gizmo, m_vp, &live);
	EXPECT_LT(glm::length(Pivot().p - live.position), 1e-5f)
	    << "a translate drags its own handle along with the object";
}

/**
 * @test A Rotate's guide keeps the snapshot's orientation while the object turns.
 *
 * The one case that can tell the two sources apart — Move and Scale never touch the
 * rotation, so for them the live transform and the snapshot agree. Constrained to the
 * gimbal X axis, whose direction is Rz(yaw) * X: at the snapshot's yaw of 0 that is
 * world +X, and at the live yaw of 90 it would be world +Y. A guide that spun with the
 * object would destroy the reference the rotation is being measured against.
 */
TEST_F(GizmoDrawBuilderTest, ARotateGuideDoesNotSpinWithTheObject)
{
	const glm::vec2 cursor = glm::vec2(kW, kH) * 0.5f + glm::vec2(100.0f, 0.0f);
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, cursor));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::X, m_vp, cursor));

	TransformSnapshot live = Anchor();
	live.rotation = glm::vec3(0.0f, 0.0f, 90.0f);  // the drag has turned it a quarter turn
	m_builder.Rebuild(m_gizmo, m_vp, &live);

	const OverlaySegment& line = AxisLine(m_builder.List());
	const glm::vec3 along = glm::normalize(line.b - line.a);
	EXPECT_NEAR(std::abs(along.x), 1.0f, 1e-4f) << "world +X: the snapshot's yaw, not the live one";
}

// ===========================================================================
// E. One constrained axis: the mode's decoration — contract 2
// ===========================================================================

/// @test Move: one line and an arrowhead at each end. Both ends, because a modal
/// constraint is a bidirectional line rather than a one-way grabbable arrow.
TEST_F(GizmoDrawBuilderTest, ConstrainedMoveDecoratesBothEndsWithArrowheads)
{
	const glm::vec2 cursor = glm::vec2(kW, kH) * 0.5f;
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, cursor));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::X, m_vp, cursor));

	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);

	EXPECT_EQ(m_builder.List().segments.size(), kMoveGuideSegments);
	EXPECT_EQ(m_builder.List().points.size(), 1u) << "the pivot only: Move has no box handle";
}

/// @test Scale: one line and a box sprite at each end, per Blender.
TEST_F(GizmoDrawBuilderTest, ConstrainedScaleDecoratesBothEndsWithBoxSprites)
{
	const glm::vec2 cursor = glm::vec2(kW, kH) * 0.5f + glm::vec2(100.0f, 0.0f);
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Scale, kUid, m_target, cursor));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::Z, m_vp, cursor));

	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);

	EXPECT_EQ(m_builder.List().segments.size(), 1u) << "a bare line: no arrowhead barbs";
	EXPECT_EQ(CountPoints(m_builder.List(), OverlayPointShape::Square), 2u);
	EXPECT_EQ(m_builder.List().points.size(), 3u) << "two boxes and the pivot";
}

/// @test Rotate: the latched line plus the ring whose plane the cursor sweeps, and no
/// end decoration at all — a tip would suggest a direction a rotation axis lacks.
TEST_F(GizmoDrawBuilderTest, ConstrainedRotateAddsARingAndNoEndDecoration)
{
	const glm::vec2 cursor = glm::vec2(kW, kH) * 0.5f + glm::vec2(100.0f, 0.0f);
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, cursor));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::Z, m_vp, cursor));

	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);

	EXPECT_EQ(m_builder.List().segments.size(), 1u + kChords);
	EXPECT_EQ(m_builder.List().points.size(), 1u) << "no arrowhead, no box: the pivot only";
}

// ===========================================================================
// F. No axis chosen: the free gestures — contract 2
// ===========================================================================

/**
 * @test An armed free Move is byte-for-byte the resting handle.
 *
 * Not a coincidence worth asserting for its own sake: the resting handle *is* "Move
 * mode, no axis chosen", so the two share every line of geometry code. If they ever
 * diverge, one of them has grown a special case.
 */
TEST_F(GizmoDrawBuilderTest, AnArmedFreeMoveMatchesTheRestingHandleExactly)
{
	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);
	const GizmoDrawList resting = m_builder.List();
	ASSERT_EQ(resting.segments.size(), 3u * kMoveGuideSegments);

	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, glm::vec2(kW, kH) * 0.5f));
	m_builder.MarkDirty();
	m_builder.Rebuild(m_gizmo, m_vp, &live);

	const GizmoDrawList& armed = m_builder.List();
	ASSERT_EQ(armed.segments.size(), resting.segments.size());
	for (size_t i = 0; i < armed.segments.size(); ++i)
	{
		EXPECT_EQ(armed.segments[i].rgba, resting.segments[i].rgba) << "segment " << i;
		EXPECT_FLOAT_EQ(armed.segments[i].width, resting.segments[i].width) << "segment " << i;
	}
}

/// @test A free Scale offers all three axes as boxed guides.
TEST_F(GizmoDrawBuilderTest, AFreeScaleOffersThreeBoxedGuides)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Scale, kUid, m_target, glm::vec2(kW, kH) * 0.5f));

	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);

	EXPECT_EQ(m_builder.List().segments.size(), 3u) << "three bare lines";
	EXPECT_EQ(CountPoints(m_builder.List(), OverlayPointShape::Square), 6u) << "two boxes per axis";
	EXPECT_EQ(m_builder.List().points.size(), 7u);
}

/**
 * @test A free Rotate draws four rings: the three gimbal planes plus the view axis.
 *
 * The view ring is the axis a bare R actually turns about, and it is drawn *outside*
 * the other three so it reads as the one enclosing them — Blender's placement. Four
 * rings is also what says no arrowhead crept in: a Move guide would add nine segments
 * that are not chords.
 */
TEST_F(GizmoDrawBuilderTest, AFreeRotateDrawsThreeGimbalRingsAndOneEnclosingViewRing)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, glm::vec2(kW, kH) * 0.5f));

	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);

	const GizmoDrawList& list = m_builder.List();
	EXPECT_EQ(list.segments.size(), 4u * kChords);
	EXPECT_EQ(CountSegments(list, kWhite), kChords)
	    << "one white ring: not one of the three coloured axes";
	EXPECT_EQ(list.points.size(), 1u) << "the pivot: a ring carries its own meaning";

	const float view = RingRadius(list, live.position,
	                             [](const OverlaySegment& s) { return s.rgba == kWhite; });
	const float gimbal = RingRadius(list, live.position,
	                                [](const OverlaySegment& s) { return s.rgba != kWhite; });
	EXPECT_GT(view, gimbal) << "the view ring encloses the three it is offered beside";
}

/**
 * @test The resting handle stays full strength; a free Rotate's gimbal rings do not.
 *
 * Brightness answers exactly one question: will dragging right now drive this axis? At
 * rest a bare G would move along all three, so they are full strength. A bare R is live
 * but turns about the *view* axis, so none of the three gimbal rings is the one being
 * dragged — they are offers, and X/Y/Z take them up mid-gesture.
 */
TEST_F(GizmoDrawBuilderTest, AFreeRotateDimsTheGimbalRingsButNotTheViewRing)
{
	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);
	const float restingWidth = AxisLine(m_builder.List()).width;

	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, glm::vec2(kW, kH) * 0.5f));
	m_builder.MarkDirty();
	m_builder.Rebuild(m_gizmo, m_vp, &live);

	for (const OverlaySegment& s : m_builder.List().segments)
	{
		if (s.rgba == kWhite)
			EXPECT_FLOAT_EQ(s.width, restingWidth) << "the view ring is the live one";
		else
			EXPECT_LT(s.width, restingWidth) << "a gimbal ring is still on offer";
	}
}

// ===========================================================================
// G. Opacity — contract 5
// ===========================================================================

/**
 * @test Nothing the gizmo draws is ever translucent, in any state.
 *
 * Dimming happens in RGB and never in alpha: a translucent guide washes out over
 * bright geometry, which was the first thing to look wrong on screen. The only alpha
 * the gizmo uses is the analytic edge fade OverlayFlag::Smooth applies, and that is
 * antialiasing rather than transparency.
 */
TEST_F(GizmoDrawBuilderTest, EveryPrimitiveIsFullyOpaqueInEveryState)
{
	const TransformSnapshot live = Anchor();
	const glm::vec2 cursor = glm::vec2(kW, kH) * 0.5f + glm::vec2(100.0f, 0.0f);

	m_builder.Rebuild(m_gizmo, m_vp, &live);
	EXPECT_TRUE(AllOpaque(m_builder.List())) << "resting";

	for (const GizmoMode mode : {GizmoMode::Move, GizmoMode::Rotate, GizmoMode::Scale})
	{
		ASSERT_TRUE(m_gizmo.Arm(mode, kUid, m_target, cursor));

		m_builder.MarkDirty();
		m_builder.Rebuild(m_gizmo, m_vp, &live);
		EXPECT_TRUE(AllOpaque(m_builder.List())) << "free, mode " << static_cast<int>(mode);

		ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::Z, m_vp, cursor));
		m_builder.MarkDirty();
		m_builder.Rebuild(m_gizmo, m_vp, &live);
		EXPECT_TRUE(AllOpaque(m_builder.List())) << "constrained, mode " << static_cast<int>(mode);
	}
}

// ===========================================================================
// H. The pixel budget — contract 4
// ===========================================================================

/**
 * @test Pulling the camera back doubles the guide's world length at double the depth.
 *
 * Which is to say the guide holds its apparent size: every length here is a fixed
 * logical-pixel budget divided by PixelsPerWorldUnit() at the pivot's depth. It is also
 * exactly why a camera change has to mark the builder dirty — the state machine has not
 * moved at all, yet every vertex has.
 */
TEST_F(GizmoDrawBuilderTest, TheGuideGrowsInWorldSpaceAsTheCameraPullsBack)
{
	const TransformSnapshot live = Anchor();
	m_builder.Rebuild(m_gizmo, m_vp, &live);
	const float near5 = SegLength(AxisLine(m_builder.List()));
	ASSERT_GT(near5, 0.0f);

	m_cam.SetPosition(glm::vec3(0.0f, -10.0f, 0.0f));  // twice the distance, same target
	m_builder.MarkDirty();
	m_builder.Rebuild(m_gizmo, m_vp, &live);
	const float far10 = SegLength(AxisLine(m_builder.List()));

	EXPECT_NEAR(far10, near5 * 2.0f, 1e-2f) << "a fixed pixel budget, not a fixed world length";
}











