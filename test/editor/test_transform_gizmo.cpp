/**
 * @file test_transform_gizmo.cpp
 * @brief CPU verification of the modal G / R / S state machine and its math.
 *
 * Nothing downstream can check any of this: the gesture writes a Transform3D and
 * the only witness is the picture on screen. Five contracts, in the order the
 * code depends on them:
 *
 *   1. GizmoRotationMatrix() IS Transform.cpp's own Rz(yaw)·Rx(pitch)·Ry(roll),
 *      minus T and S. Two independent products of the same three angles is the
 *      one way this feature can be subtly, invisibly wrong — the guide would
 *      point somewhere the object does not turn.
 *   2. GizmoEulerFromRotation() inverts it exactly, including at gimbal lock,
 *      and returns the *canonical* representative. A free rotate rewrites the
 *      whole triple, so a caller that expects its numbers back unchanged is
 *      wrong, not the solve.
 *   3. There are two axis sets, not one. Rotate uses the gimbal set (what each
 *      stored Euler component actually turns about); Move and Scale use the true
 *      local frame. They agree only in special cases, enumerated below.
 *   4. Every drag re-derives from the before-snapshot, never from what it wrote
 *      last frame. A cursor round trip must therefore land on the *exact*
 *      starting value — asserted with == , not a tolerance.
 *   5. Re-Constrain() normalizes. Dropping a constraint (W) or switching axis
 *      re-latches every anchor, so the one Drag() the controller issues next
 *      reproduces the before-state and *undoes* the previous axis's partial
 *      transform rather than compounding with it.
 *
 * No GPU, no Scene, no event queue: a bare Camera drives an EditorViewport and a
 * bare Transform3D stands in for the object, exactly as the controller hands one in.
 */

#include <gtest/gtest.h>

#include "editor/viewport/TransformGizmo.h"
#include "editor/viewport/EditorViewport.h"
#include "scene/Camera.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace neurus;

namespace {

constexpr uint32_t kW = 800u;
constexpr uint32_t kH = 600u;
constexpr int      kUid = 7;  ///< Any non-zero uid; Arm() rejects 0.

/// @brief Largest absolute component-wise difference between two vectors.
float MaxDiff(const glm::vec3& a, const glm::vec3& b)
{
	const glm::vec3 d = glm::abs(a - b);
	return std::max(d.x, std::max(d.y, d.z));
}

} // namespace

// ===========================================================================
// A. The rotation matrix and its inverse
// ===========================================================================

/**
 * @test GizmoRotationMatrix is Transform3D's own product, minus T and S.
 *
 * Contract 1. Both sides build Rz(yaw)·Rx(pitch)·Ry(roll) from the same stored
 * triple, independently. If they ever drift apart, every guide points somewhere
 * the object does not turn — and the object still turns, so nothing throws.
 */
TEST(GizmoMath, RotationMatrix_MatchesTransform3DsModelMatrix)
{
	for (const glm::vec3& euler : {glm::vec3(0.0f),
	                               glm::vec3(30.0f, 0.0f, 0.0f),
	                               glm::vec3(0.0f, 45.0f, 0.0f),
	                               glm::vec3(0.0f, 0.0f, -60.0f),
	                               glm::vec3(20.0f, 35.0f, -50.0f)})
	{
		Transform3D t;
		t.SetRotation(euler);  // position 0, scale 1, so the model matrix is pure R

		const glm::mat3 mine = GizmoRotationMatrix(euler);
		const glm::mat3 theirs{t.GetModelMatrix()};
		for (int c = 0; c < 3; ++c)
			EXPECT_LT(MaxDiff(mine[c], theirs[c]), 1e-5f) << "column " << c << " of " << euler.x;
	}
}

/// @test It is a rotation: orthonormal and right-handed.
TEST(GizmoMath, RotationMatrix_IsOrthonormalAndRightHanded)
{
	const glm::mat3 r = GizmoRotationMatrix(glm::vec3(20.0f, 35.0f, -50.0f));
	EXPECT_NEAR(glm::determinant(r), 1.0f, 1e-5f);
	for (int c = 0; c < 3; ++c)
		EXPECT_NEAR(glm::length(r[c]), 1.0f, 1e-5f);
	EXPECT_NEAR(glm::dot(r[0], r[1]), 0.0f, 1e-5f);
	EXPECT_NEAR(glm::dot(r[1], r[2]), 0.0f, 1e-5f);
}

/// @test Euler → matrix → Euler is the identity for every non-degenerate triple.
TEST(GizmoMath, EulerFromRotation_RoundTripsAwayFromGimbalLock)
{
	for (const glm::vec3& euler : {glm::vec3(0.0f),
	                               glm::vec3(30.0f, 0.0f, 0.0f),
	                               glm::vec3(-89.0f, 0.0f, 0.0f),
	                               glm::vec3(0.0f, 170.0f, 0.0f),
	                               glm::vec3(0.0f, 0.0f, -179.0f),
	                               glm::vec3(20.0f, 35.0f, -50.0f),
	                               glm::vec3(-15.0f, 120.0f, 95.0f)})
	{
		const glm::vec3 back = GizmoEulerFromRotation(GizmoRotationMatrix(euler));
		EXPECT_LT(MaxDiff(back, euler), 1e-3f) << "round trip of (" << euler.x << ", "
		                                       << euler.y << ", " << euler.z << ")";
	}
}

/**
 * @test An out-of-range triple comes back canonicalized, same orientation.
 *
 * Contract 2, and the reason a free rotate rewrites all three numbers instead of
 * one: yaw 200 and yaw -160 are the same rotation, and the solve has to pick one.
 */
TEST(GizmoMath, EulerFromRotation_ReturnsTheCanonicalRepresentative)
{
	const glm::vec3 wound{0.0f, 0.0f, 200.0f};
	const glm::vec3 back = GizmoEulerFromRotation(GizmoRotationMatrix(wound));

	EXPECT_NEAR(back.z, -160.0f, 1e-3f) << "the same orientation, different numbers";

	// Same matrix, which is the property that actually matters.
	const glm::mat3 a = GizmoRotationMatrix(wound);
	const glm::mat3 b = GizmoRotationMatrix(back);
	for (int c = 0; c < 3; ++c)
		EXPECT_LT(MaxDiff(a[c], b[c]), 1e-5f);
}

/**
 * @test At pitch = ±90 roll is pinned to zero and the matrix still round-trips.
 *
 * Yaw and roll turn about the same world axis there, so only their sum is
 * observable. Pinning roll picks the one representative; the matrix is what has
 * to survive, not the numbers.
 */
TEST(GizmoMath, EulerFromRotation_PinsRollToZeroAtGimbalLock)
{
	for (const float pitch : {90.0f, -90.0f})
	{
		const glm::vec3 locked{pitch, 30.0f, 40.0f};
		const glm::vec3 back = GizmoEulerFromRotation(GizmoRotationMatrix(locked));

		EXPECT_NEAR(back.x, pitch, 1e-3f);
		EXPECT_FLOAT_EQ(back.y, 0.0f) << "roll is pinned, not guessed";

		const glm::mat3 a = GizmoRotationMatrix(locked);
		const glm::mat3 b = GizmoRotationMatrix(back);
		for (int c = 0; c < 3; ++c)
			EXPECT_LT(MaxDiff(a[c], b[c]), 1e-4f) << "column " << c << " at pitch " << pitch;
	}
}

// ===========================================================================
// B. Two axis sets, not one
// ===========================================================================

/// @test An unrotated object has both sets reduce to world XYZ.
TEST(GizmoMath, AxisDirection_IsWorldXYZForAnUnrotatedObject)
{
	for (const GizmoMode mode : {GizmoMode::Move, GizmoMode::Rotate, GizmoMode::Scale})
	{
		EXPECT_LT(MaxDiff(GizmoAxisDirection(mode, GizmoAxis::X, glm::vec3(0.0f)),
		                  glm::vec3(1.0f, 0.0f, 0.0f)), 1e-5f);
		EXPECT_LT(MaxDiff(GizmoAxisDirection(mode, GizmoAxis::Y, glm::vec3(0.0f)),
		                  glm::vec3(0.0f, 1.0f, 0.0f)), 1e-5f);
		EXPECT_LT(MaxDiff(GizmoAxisDirection(mode, GizmoAxis::Z, glm::vec3(0.0f)),
		                  glm::vec3(0.0f, 0.0f, 1.0f)), 1e-5f);
	}
}

/// @test Either enum being None yields the zero vector, never a guessed axis.
TEST(GizmoMath, AxisDirection_ZeroWhenEitherEnumIsNone)
{
	EXPECT_FLOAT_EQ(glm::length(GizmoAxisDirection(GizmoMode::None, GizmoAxis::X,
	                                              glm::vec3(0.0f))), 0.0f);
	EXPECT_FLOAT_EQ(glm::length(GizmoAxisDirection(GizmoMode::Move, GizmoAxis::None,
	                                              glm::vec3(10.0f, 20.0f, 30.0f))), 0.0f);
}

/**
 * @test Y is the one axis the gimbal and local sets always share.
 *
 * Roll is the innermost rotation, so Ry leaves its own axis alone: the gimbal
 * axis Rz·Rx·Y and the local column (Rz·Rx·Ry)·Y are the same vector for every
 * triple. It is also exactly Transform3D::GetDirection().
 */
TEST(GizmoMath, AxisDirection_YAgreesBetweenBothSetsForEveryTriple)
{
	for (const glm::vec3& euler : {glm::vec3(20.0f, 35.0f, -50.0f),
	                               glm::vec3(-70.0f, 160.0f, 15.0f),
	                               glm::vec3(0.0f, 0.0f, 0.0f)})
	{
		const glm::vec3 gimbal = GizmoAxisDirection(GizmoMode::Rotate, GizmoAxis::Y, euler);
		const glm::vec3 local  = GizmoAxisDirection(GizmoMode::Move, GizmoAxis::Y, euler);
		EXPECT_LT(MaxDiff(gimbal, local), 1e-5f);

		Transform3D t;
		t.SetRotation(euler);
		EXPECT_LT(MaxDiff(local, t.GetDirection()), 1e-5f) << "and it is GetDirection()";
	}
}

/**
 * @test X and Z do NOT agree between the sets, and that is the point.
 *
 * X parts company as soon as roll is non-zero (Rx fixes X, Ry does not); Z parts
 * company as soon as *pitch* is non-zero (Rz fixes world Z, Rx does not). Using
 * one set for both would drag a constrained Move along a line the guide is not
 * drawn on, which is why the two derivations are separate branches.
 */
TEST(GizmoMath, AxisDirection_XAndZDivergeBetweenTheSets)
{
	const glm::vec3 rollOnly{0.0f, 40.0f, 0.0f};
	EXPECT_GT(MaxDiff(GizmoAxisDirection(GizmoMode::Rotate, GizmoAxis::X, rollOnly),
	                  GizmoAxisDirection(GizmoMode::Move, GizmoAxis::X, rollOnly)), 0.1f)
	    << "roll alone separates the two X axes";

	const glm::vec3 pitchOnly{40.0f, 0.0f, 0.0f};
	EXPECT_LT(MaxDiff(GizmoAxisDirection(GizmoMode::Rotate, GizmoAxis::X, pitchOnly),
	                  GizmoAxisDirection(GizmoMode::Move, GizmoAxis::X, pitchOnly)), 1e-5f)
	    << "but pitch alone does not: Rx fixes X";

	EXPECT_GT(MaxDiff(GizmoAxisDirection(GizmoMode::Rotate, GizmoAxis::Z, pitchOnly),
	                  GizmoAxisDirection(GizmoMode::Move, GizmoAxis::Z, pitchOnly)), 0.1f)
	    << "pitch separates the two Z axes";
}

/// @test The gimbal Z axis is world Z whatever the object's orientation.
TEST(GizmoMath, AxisDirection_GimbalZIsAlwaysWorldZ)
{
	const glm::vec3 axis =
	    GizmoAxisDirection(GizmoMode::Rotate, GizmoAxis::Z, glm::vec3(20.0f, 35.0f, -50.0f));
	EXPECT_LT(MaxDiff(axis, glm::vec3(0.0f, 0.0f, 1.0f)), 1e-5f)
	    << "yaw is the outermost rotation, so nothing tilts its axis";
}

// ===========================================================================
// C. The gesture: fixture
// ===========================================================================

/**
 * @brief An eye at (0,-5,0) looking at the origin, Z up — the project convention.
 *
 * World +X is screen right and +Z is screen up at that pose, and world +Y is pure
 * depth. That is deliberate: it makes world Y the axis a constrained Move must
 * *refuse*, and the view axis a free Rotate turns about, with no matrix algebra in
 * the assertions.
 */
class TransformGizmoTest : public ::testing::Test
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

	/// @brief The pixel the pivot at the world origin projects to.
	glm::vec2 Centre() const { return glm::vec2(kW, kH) * 0.5f; }

	Camera         m_cam;
	EditorViewport m_vp;
	TransformGizmo m_gizmo;
	Transform3D    m_target;  ///< Identity: origin, no rotation, unit scale.
};

// ===========================================================================
// D. Arm / Cancel / Disarm
// ===========================================================================

/// @test A mode of None or a uid of 0 arms nothing and changes nothing.
TEST_F(TransformGizmoTest, Arm_RejectsNoModeAndNoObject)
{
	EXPECT_FALSE(m_gizmo.Arm(GizmoMode::None, kUid, m_target, Centre()));
	EXPECT_FALSE(m_gizmo.Arm(GizmoMode::Move, 0, m_target, Centre()));
	EXPECT_FALSE(m_gizmo.IsActive());
}

/// @test Arm snapshots the whole triple and leaves the gesture free, not idle.
TEST_F(TransformGizmoTest, Arm_SnapshotsTheWholeTripleAndLeavesNoAxis)
{
	m_target.SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	m_target.SetRotation(glm::vec3(10.0f, 20.0f, 30.0f));
	m_target.SetScale(glm::vec3(2.0f, 3.0f, 4.0f));

	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, Centre()));
	EXPECT_TRUE(m_gizmo.IsActive());
	EXPECT_EQ(m_gizmo.Mode(), GizmoMode::Rotate);
	EXPECT_EQ(m_gizmo.Axis(), GizmoAxis::None) << "armed with no axis is the free gesture";
	EXPECT_EQ(m_gizmo.ObjectUid(), kUid);

	// The whole triple, not just the component this mode edits: that is what makes
	// Cancel() a literal restore.
	EXPECT_EQ(m_gizmo.Before().position, glm::vec3(1.0f, 2.0f, 3.0f));
	EXPECT_EQ(m_gizmo.Before().rotation, glm::vec3(10.0f, 20.0f, 30.0f));
	EXPECT_EQ(m_gizmo.Before().scale, glm::vec3(2.0f, 3.0f, 4.0f));
}

/// @test Cancel restores every component exactly; Disarm keeps what is there.
TEST_F(TransformGizmoTest, CancelRestoresAndDisarmKeeps)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, Centre()));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::X, m_vp, Centre()));
	ASSERT_TRUE(m_gizmo.Drag(m_vp, Centre() + glm::vec2(120.0f, 0.0f), m_target));
	ASSERT_NE(m_target.GetPosition(), glm::vec3(0.0f));

	m_gizmo.Cancel(m_target);
	EXPECT_EQ(m_target.GetPosition(), glm::vec3(0.0f));
	EXPECT_TRUE(m_gizmo.IsActive()) << "Cancel restores; only Disarm ends the gesture";

	m_gizmo.Disarm();
	EXPECT_FALSE(m_gizmo.IsActive());
	EXPECT_EQ(m_gizmo.Axis(), GizmoAxis::None);
	EXPECT_EQ(m_gizmo.ObjectUid(), 0);
}

/// @test Cancel and Drag on an inactive gizmo do nothing at all.
TEST_F(TransformGizmoTest, InactiveGizmoIgnoresCancelAndDrag)
{
	m_target.SetPosition(glm::vec3(4.0f, 5.0f, 6.0f));
	m_gizmo.Cancel(m_target);
	EXPECT_EQ(m_target.GetPosition(), glm::vec3(4.0f, 5.0f, 6.0f));
	EXPECT_FALSE(m_gizmo.Drag(m_vp, Centre(), m_target));
}

/// @test Constrain needs a usable viewport; without one it refuses and stays free.
TEST_F(TransformGizmoTest, Constrain_RefusedWithoutAValidViewport)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, Centre()));

	EditorViewport blank;
	EXPECT_FALSE(m_gizmo.Constrain(GizmoAxis::X, blank, Centre()));
	EXPECT_EQ(m_gizmo.Axis(), GizmoAxis::None);
	EXPECT_TRUE(m_gizmo.IsActive()) << "a refusal leaves the mode armed";
}

// ===========================================================================
// E. Constrained Move
// ===========================================================================

/**
 * @test An axis along the view ray is refused, and the gesture survives it.
 *
 * At this pose world Y *is* the view ray, so the closest-point solve is
 * meaningless and a pixel of cursor motion would be metres of travel. X is
 * perpendicular and accepted, which is what proves the refusal is about the
 * geometry and not about Constrain() being broken.
 */
TEST_F(TransformGizmoTest, ConstrainMove_RefusesTheAxisAlongTheViewRay)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, Centre()));

	EXPECT_FALSE(m_gizmo.Constrain(GizmoAxis::Y, m_vp, Centre()));
	EXPECT_EQ(m_gizmo.Axis(), GizmoAxis::None) << "refused: still armed, still axis-less";

	EXPECT_TRUE(m_gizmo.Constrain(GizmoAxis::X, m_vp, Centre()));
	EXPECT_EQ(m_gizmo.Axis(), GizmoAxis::X);
}

/// @test A constrained Move stays on its line: only X changes, and in the right sign.
TEST_F(TransformGizmoTest, DragMove_MovesAlongTheConstraintLineOnly)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, Centre()));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::X, m_vp, Centre()));

	ASSERT_TRUE(m_gizmo.Drag(m_vp, Centre() + glm::vec2(150.0f, 0.0f), m_target));
	EXPECT_GT(m_target.GetPosition().x, 0.0f) << "screen right is world +X at this pose";
	EXPECT_FLOAT_EQ(m_target.GetPosition().y, 0.0f);
	EXPECT_FLOAT_EQ(m_target.GetPosition().z, 0.0f);
}

/**
 * @test A cursor round trip lands on the EXACT starting value.
 *
 * Contract 4. Asserted with == rather than a tolerance, which is only defensible
 * because the drag recomputes `before + (t - grabAnchorT) * axis` from the
 * snapshot: at the anchor pixel the two parameters are the same float, so the
 * offset is identically zero. A drag that integrated deltas would drift here.
 */
TEST_F(TransformGizmoTest, DragMove_IsDriftFreeAcrossARoundTrip)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, Centre()));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::X, m_vp, Centre()));

	for (const float dx : {40.0f, 90.0f, 210.0f, 90.0f, -160.0f})
		m_gizmo.Drag(m_vp, Centre() + glm::vec2(dx, 0.0f), m_target);
	ASSERT_NE(m_target.GetPosition(), glm::vec3(0.0f));

	ASSERT_TRUE(m_gizmo.Drag(m_vp, Centre(), m_target));
	EXPECT_EQ(m_target.GetPosition(), glm::vec3(0.0f)) << "exactly, not approximately";
}

// ===========================================================================
// F. Constrained Rotate
// ===========================================================================

/// @test A constrained Rotate adds to exactly one stored Euler component.
TEST_F(TransformGizmoTest, DragRotate_AddsToOneStoredComponentOnly)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, Centre()));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::Z, m_vp, Centre() + glm::vec2(100.0f, 0.0f)));

	ASSERT_TRUE(m_gizmo.Drag(m_vp, Centre() + glm::vec2(0.0f, -100.0f), m_target));

	// `before + theta` on the yaw component: no matrix round trip, so the two it does
	// not touch are bitwise unchanged and the PropertyPanel keeps a readable number.
	EXPECT_FLOAT_EQ(m_target.GetRotation().x, 0.0f);
	EXPECT_FLOAT_EQ(m_target.GetRotation().y, 0.0f);
	EXPECT_NEAR(std::abs(m_target.GetRotation().z), 90.0f, 1e-3f);
}

/**
 * @test Two full turns accumulate to two full turns, not to a wrapped remainder.
 *
 * The bearing is an atan2 and therefore lives on (-pi, pi]; only the unwrapped
 * accumulation makes a sweep past 180 degrees mean what it looks like.
 */
TEST_F(TransformGizmoTest, DragRotate_AccumulatesPastMultipleTurns)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, Centre()));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::Z, m_vp, Centre() + glm::vec2(100.0f, 0.0f)));

	// 24 steps of 15 degrees apiece: one full turn, sampled finely enough that no single
	// step is ambiguous about which way it went.
	for (int i = 1; i <= 24; ++i)
	{
		const float a = glm::radians(15.0f * static_cast<float>(i));
		m_gizmo.Drag(m_vp, Centre() + glm::vec2(std::cos(a), std::sin(a)) * 100.0f, m_target);
	}
	EXPECT_NEAR(std::abs(m_target.GetRotation().z), 360.0f, 1e-2f);

	for (int i = 25; i <= 48; ++i)
	{
		const float a = glm::radians(15.0f * static_cast<float>(i));
		m_gizmo.Drag(m_vp, Centre() + glm::vec2(std::cos(a), std::sin(a)) * 100.0f, m_target);
	}
	EXPECT_NEAR(std::abs(m_target.GetRotation().z), 720.0f, 1e-2f);
}

// ===========================================================================
// G. Constrained Scale
// ===========================================================================

/// @test A constrained Scale is the radius ratio on one component, 1 at the anchor.
TEST_F(TransformGizmoTest, DragScale_IsTheRadiusRatioOnOneComponent)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Scale, kUid, m_target, Centre()));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::X, m_vp, Centre() + glm::vec2(100.0f, 0.0f)));

	ASSERT_TRUE(m_gizmo.Drag(m_vp, Centre() + glm::vec2(200.0f, 0.0f), m_target));
	EXPECT_NEAR(m_target.GetScale().x, 2.0f, 1e-3f) << "twice the radius is twice the scale";
	EXPECT_FLOAT_EQ(m_target.GetScale().y, 1.0f);
	EXPECT_FLOAT_EQ(m_target.GetScale().z, 1.0f);

	// Back to the anchor *radius* on the far side of the pivot: the factor is a length
	// ratio, so where on the circle the cursor sits cannot matter.
	m_gizmo.Drag(m_vp, Centre() + glm::vec2(0.0f, -100.0f), m_target);
	EXPECT_NEAR(m_target.GetScale().x, 1.0f, 1e-4f) << "the factor depends on radius alone";
}

// ===========================================================================
// H. The free gestures — what a bare G / R / S does
// ===========================================================================

/// @test A bare G slides the object across the view plane, depth untouched.
TEST_F(TransformGizmoTest, DragMoveFree_SlidesAcrossTheViewPlane)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, Centre()));

	// No axis, yet already live: that is the whole of Blender's bare G.
	ASSERT_EQ(m_gizmo.Axis(), GizmoAxis::None);
	ASSERT_TRUE(m_gizmo.Drag(m_vp, Centre() + glm::vec2(100.0f, -80.0f), m_target));

	EXPECT_GT(m_target.GetPosition().x, 0.0f) << "screen right is world +X";
	EXPECT_GT(m_target.GetPosition().z, 0.0f) << "screen up is world +Z (smaller pixel y)";
	EXPECT_NEAR(m_target.GetPosition().y, 0.0f, 1e-3f) << "the plane is parallel to the screen";
}

/// @test At the anchor pixel a free Move reports no change at all — no opening snap.
TEST_F(TransformGizmoTest, DragMoveFree_IsExactlyZeroAtTheAnchor)
{
	const glm::vec2 anchor = Centre() + glm::vec2(60.0f, 40.0f);
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, anchor));

	EXPECT_FALSE(m_gizmo.Drag(m_vp, anchor, m_target)) << "nothing moved, so nothing to report";
	EXPECT_EQ(m_target.GetPosition(), glm::vec3(0.0f));
}

/// @test A bare S scales all three components by one factor, preserving shape.
TEST_F(TransformGizmoTest, DragScaleFree_IsUniform)
{
	m_target.SetScale(glm::vec3(1.0f, 2.0f, 3.0f));
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Scale, kUid, m_target, Centre() + glm::vec2(100.0f, 0.0f)));

	ASSERT_TRUE(m_gizmo.Drag(m_vp, Centre() + glm::vec2(200.0f, 0.0f), m_target));
	EXPECT_NEAR(m_target.GetScale().x, 2.0f, 1e-3f);
	EXPECT_NEAR(m_target.GetScale().y, 4.0f, 1e-3f);
	EXPECT_NEAR(m_target.GetScale().z, 6.0f, 1e-3f);
}

/**
 * @test A bare R turns about the view axis, which here is world Y — i.e. roll.
 *
 * The one gesture that leaves the `before + theta` identity: the view axis is not
 * a one-parameter subgroup of the stored triple, so the angle is composed as a
 * matrix and read back. At this camera pose the view axis is exactly world +Y, so
 * a quarter turn has to land on roll = -90 and leave pitch and yaw at zero — the
 * assertion that would catch a wrong multiplication order or a wrong sign latch.
 */
TEST_F(TransformGizmoTest, DragRotateFree_TurnsAboutTheViewAxis)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, Centre() + glm::vec2(100.0f, 0.0f)));

	ASSERT_TRUE(m_gizmo.Drag(m_vp, Centre() + glm::vec2(0.0f, -100.0f), m_target));
	EXPECT_NEAR(m_target.GetRotation().x, 0.0f, 1e-3f) << "pitch";
	EXPECT_NEAR(m_target.GetRotation().y, -90.0f, 1e-2f) << "roll: the view axis is world +Y";
	EXPECT_NEAR(m_target.GetRotation().z, 0.0f, 1e-3f) << "yaw";
}

/**
 * @test At the anchor a free Rotate writes the snapshot back rather than composing.
 *
 * Composing at zero angle would canonicalise the triple, so an unturned gesture
 * would rewrite equivalent-but-different numbers and leave the controller
 * recording an undo entry for nothing. The wound-up triple below is what makes
 * the difference observable: a round trip through the matrix would return -160.
 */
TEST_F(TransformGizmoTest, DragRotateFree_AtTheAnchorKeepsTheStoredNumbers)
{
	const glm::vec3 wound{0.0f, 0.0f, 200.0f};
	m_target.SetRotation(wound);

	const glm::vec2 anchor = Centre() + glm::vec2(100.0f, 0.0f);
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, anchor));

	EXPECT_FALSE(m_gizmo.Drag(m_vp, anchor, m_target)) << "no angle, so no change to report";
	EXPECT_EQ(m_target.GetRotation(), wound) << "not canonicalised behind the user's back";
}

/// @test Inside the dead zone around the pivot the bearing is ignored, not guessed.
TEST_F(TransformGizmoTest, DragRotateFree_HoldsInsideTheDeadZone)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, Centre() + glm::vec2(100.0f, 0.0f)));

	EXPECT_FALSE(m_gizmo.Drag(m_vp, Centre() + glm::vec2(2.0f, 1.0f), m_target));
	EXPECT_EQ(m_target.GetRotation(), glm::vec3(0.0f));
}

// ===========================================================================
// I. Re-Constrain normalizes — the whole of W, and of an axis switch
// ===========================================================================

/**
 * @test W drops a Move's axis and the next drag undoes what that axis applied.
 *
 * Contract 5. Constrain(None) re-latches the free anchor at the cursor, so the one
 * Drag() the controller issues at that same cursor reproduces the before-state —
 * exactly, since both ends of the free solve are the same pixel. Without this,
 * leaving a constraint would keep the partial translate and silently compound it.
 */
TEST_F(TransformGizmoTest, ConstrainNone_NormalizesAConstrainedMoveBackToBefore)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, Centre()));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::X, m_vp, Centre()));

	const glm::vec2 cursor = Centre() + glm::vec2(150.0f, 0.0f);
	ASSERT_TRUE(m_gizmo.Drag(m_vp, cursor, m_target));
	ASSERT_NE(m_target.GetPosition(), glm::vec3(0.0f));

	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::None, m_vp, cursor));
	EXPECT_EQ(m_gizmo.Axis(), GizmoAxis::None);
	EXPECT_TRUE(m_gizmo.IsActive()) << "W changes the constraint, it does not end the gesture";

	ASSERT_TRUE(m_gizmo.Drag(m_vp, cursor, m_target));
	EXPECT_EQ(m_target.GetPosition(), glm::vec3(0.0f));
}

/// @test W on a constrained Rotate restores the stored triple verbatim.
TEST_F(TransformGizmoTest, ConstrainNone_NormalizesAConstrainedRotateBackToBefore)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Rotate, kUid, m_target, Centre()));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::Z, m_vp, Centre() + glm::vec2(100.0f, 0.0f)));

	const glm::vec2 cursor = Centre() + glm::vec2(0.0f, -100.0f);
	ASSERT_TRUE(m_gizmo.Drag(m_vp, cursor, m_target));
	ASSERT_NE(m_target.GetRotation(), glm::vec3(0.0f));

	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::None, m_vp, cursor));
	ASSERT_TRUE(m_gizmo.Drag(m_vp, cursor, m_target));
	EXPECT_EQ(m_target.GetRotation(), glm::vec3(0.0f)) << "the snapshot, not a recomposition";
}

/// @test W on a constrained Scale restores the whole triple.
TEST_F(TransformGizmoTest, ConstrainNone_NormalizesAConstrainedScaleBackToBefore)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Scale, kUid, m_target, Centre()));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::X, m_vp, Centre() + glm::vec2(100.0f, 0.0f)));

	const glm::vec2 cursor = Centre() + glm::vec2(250.0f, 0.0f);
	ASSERT_TRUE(m_gizmo.Drag(m_vp, cursor, m_target));
	ASSERT_NE(m_target.GetScale(), glm::vec3(1.0f));

	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::None, m_vp, cursor));
	ASSERT_TRUE(m_gizmo.Drag(m_vp, cursor, m_target));
	EXPECT_EQ(m_target.GetScale(), glm::vec3(1.0f)) << "the radius ratio is exactly 1 again";
}

/**
 * @test Switching axis mid-gesture undoes the previous axis rather than compounding.
 *
 * The same re-latch as W, but landing on a different axis, which is the case a user
 * actually hits: G, X, drag, then Z. The normalizing drag has to cancel the X travel
 * before any Z travel is measured, or the object keeps an offset no axis accounts for.
 */
TEST_F(TransformGizmoTest, Constrain_SwitchingAxisUndoesThePreviousAxisTravel)
{
	ASSERT_TRUE(m_gizmo.Arm(GizmoMode::Move, kUid, m_target, Centre()));
	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::X, m_vp, Centre()));

	const glm::vec2 cursor = Centre() + glm::vec2(150.0f, 0.0f);
	ASSERT_TRUE(m_gizmo.Drag(m_vp, cursor, m_target));
	ASSERT_GT(m_target.GetPosition().x, 0.0f);

	ASSERT_TRUE(m_gizmo.Constrain(GizmoAxis::Z, m_vp, cursor));
	EXPECT_EQ(m_gizmo.Axis(), GizmoAxis::Z);

	ASSERT_TRUE(m_gizmo.Drag(m_vp, cursor, m_target));
	EXPECT_EQ(m_target.GetPosition(), glm::vec3(0.0f)) << "the X travel is gone, not carried over";
}











