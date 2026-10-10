/**
 * @file test_editor_viewport.cpp
 * @brief CPU verification of EditorViewport: projection, rays, pixel scale.
 *
 * Every gizmo guide is *sized* through this class and every modal drag is
 * *solved* through it, so a fault here is invisible in isolation and wrong
 * everywhere at once. Four contracts nothing downstream can check:
 *
 *   1. Exactly one Y flip exists in the whole chain, and it lives in the
 *      projection matrix (Camera.cpp's `proj[1][1] *= -1`). Mapping NDC to a
 *      top-left-origin pixel must therefore add none of its own. A second flip
 *      still passes every round-trip test — it cancels itself — while dragging
 *      every gizmo the wrong way, so the sign has to be pinned against world
 *      space directly.
 *   2. Project() and RayThrough() are inverses. The gizmo's free Move solves the
 *      cursor against a world plane and its constrained Move against a world
 *      line, both starting from a ray this class built.
 *   3. PixelsPerWorldUnit() is aspect-independent. It is the divisor that turns
 *      every fixed pixel budget in GizmoDrawBuilder into world units, so an
 *      aspect term in it would resize the whole handle with the window shape.
 *   4. A point behind the eye is *data*, not an exception. Dividing by a
 *      negative w mirrors the point through the viewport centre and looks
 *      entirely plausible, so `visible` is the only thing separating the two.
 *
 * No GPU and no Scene: a bare Camera is pushed in, exactly as the Editor does.
 */

#include <gtest/gtest.h>

#include "editor/viewport/EditorViewport.h"
#include "scene/Camera.h"
#include "scene/Sprite.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace neurus;

namespace {

constexpr uint32_t kW = 800u;
constexpr uint32_t kH = 600u;

/// @brief Distance from @p p to the infinite line through the ray.
float DistanceToRay(const ScreenRay& ray, const glm::vec3& p)
{
	const glm::vec3 d = p - ray.origin;
	return glm::length(d - ray.direction * glm::dot(d, ray.direction));
}

} // namespace

/**
 * @brief A viewport looking down +Y from (0,-5,0), Z up — the project convention.
 *
 * At that pose world +X is screen right and world +Z is screen up, which is what
 * lets the sign tests below name an axis instead of a matrix entry.
 */
class EditorViewportTest : public ::testing::Test
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

	Camera m_cam;
	EditorViewport m_vp;
};

// ===========================================================================
// A. Validity is data
// ===========================================================================

/// @test No camera and no size are both normal states, not errors.
TEST(EditorViewport, Invalid_WithoutCameraOrSize)
{
	EditorViewport vp;
	EXPECT_FALSE(vp.IsValid());
	EXPECT_FALSE(vp.Project(glm::vec3(0.0f)).visible);
	EXPECT_FALSE(vp.RayThrough(glm::vec2(1.0f)).valid);

	Camera cam;
	vp.SetCamera(&cam);
	EXPECT_FALSE(vp.IsValid()) << "a camera alone is not enough; the size is still zero";

	vp.SetViewportSize(glm::uvec2(kW, kH), glm::uvec2(kW, kH));
	EXPECT_TRUE(vp.IsValid());
}

/// @test A point behind the eye is invisible rather than mirrored through centre.
TEST_F(EditorViewportTest, Project_BehindTheEyeIsInvisible)
{
	const ScreenPoint behind = m_vp.Project(glm::vec3(0.0f, -10.0f, 0.0f));
	EXPECT_FALSE(behind.visible);
	EXPECT_LE(behind.viewDepth, 0.0f);
}

// ===========================================================================
// B. Project: one Y flip, pinned against world space
// ===========================================================================

/// @test The look-at target lands on the viewport centre.
TEST_F(EditorViewportTest, Project_TargetLandsOnCentre)
{
	const ScreenPoint c = m_vp.Project(glm::vec3(0.0f));
	ASSERT_TRUE(c.visible);
	EXPECT_NEAR(c.pixel.x, static_cast<float>(kW) * 0.5f, 1e-2f);
	EXPECT_NEAR(c.pixel.y, static_cast<float>(kH) * 0.5f, 1e-2f);
	EXPECT_NEAR(c.viewDepth, 5.0f, 1e-3f) << "viewDepth is the clip-space w: the eye distance";
}

/**
 * @test A point above the world axis gets the SMALLER pixel y.
 *
 * The whole point of the class doc's "no Y flip lives here": pixels are
 * top-left-origin, so up on screen is *down* in pixel y. A second flip
 * somewhere in the chain inverts this while leaving every round trip intact.
 */
TEST_F(EditorViewportTest, Project_PointAboveTheAxisHasSmallerPixelY)
{
	const ScreenPoint origin = m_vp.Project(glm::vec3(0.0f));
	const ScreenPoint up     = m_vp.Project(glm::vec3(0.0f, 0.0f, 1.0f));
	ASSERT_TRUE(origin.visible && up.visible);
	EXPECT_LT(up.pixel.y, origin.pixel.y);
	EXPECT_NEAR(up.pixel.x, origin.pixel.x, 1e-2f) << "a pure +Z offset must not move x";
}

/// @test World +X is screen right: the larger pixel x.
TEST_F(EditorViewportTest, Project_PointRightOfTheAxisHasLargerPixelX)
{
	const ScreenPoint origin = m_vp.Project(glm::vec3(0.0f));
	const ScreenPoint right  = m_vp.Project(glm::vec3(1.0f, 0.0f, 0.0f));
	ASSERT_TRUE(origin.visible && right.visible);
	EXPECT_GT(right.pixel.x, origin.pixel.x);
	EXPECT_NEAR(right.pixel.y, origin.pixel.y, 1e-2f);
}

// ===========================================================================
// C. RayThrough is Project's inverse
// ===========================================================================

/// @test Unprojecting a pixel Project() produced gives a ray through that point.
TEST_F(EditorViewportTest, RayThrough_InvertsProjectForOffCentrePoints)
{
	for (const glm::vec3& world : {glm::vec3(0.0f),
	                               glm::vec3(1.5f, 0.5f, -2.0f),
	                               glm::vec3(-3.0f, 2.0f, 1.0f)})
	{
		const ScreenPoint sp = m_vp.Project(world);
		ASSERT_TRUE(sp.visible) << "test point must be in front of the eye";

		const ScreenRay ray = m_vp.RayThrough(sp.pixel);
		ASSERT_TRUE(ray.valid);
		EXPECT_NEAR(glm::length(ray.direction), 1.0f, 1e-4f);
		EXPECT_LT(DistanceToRay(ray, world), 1e-3f);
	}
}

/// @test The ray through the centre pixel points along the view axis.
TEST_F(EditorViewportTest, RayThrough_CentrePixelFollowsTheViewAxis)
{
	const ScreenRay ray = m_vp.RayThrough(glm::vec2(kW, kH) * 0.5f);
	ASSERT_TRUE(ray.valid);
	EXPECT_NEAR(glm::dot(ray.direction, m_vp.ViewAxis()), 1.0f, 1e-4f);
}

// ===========================================================================
// D. PixelsPerWorldUnit
// ===========================================================================

/**
 * @test Window *width* cannot change the pixel scale.
 *
 * GizmoDrawBuilder divides every fixed pixel budget by this, so an aspect term
 * here would make the whole handle grow and shrink with the window's shape.
 */
TEST(EditorViewport, PixelsPerWorldUnit_IsAspectIndependent)
{
	Camera wide;
	wide.ChangeCamRatio(1600.0f, 600.0f);
	wide.SetPosition(glm::vec3(0.0f, -5.0f, 0.0f));
	wide.SetTarPos(glm::vec3(0.0f));

	Camera narrow;
	narrow.ChangeCamRatio(400.0f, 600.0f);
	narrow.SetPosition(glm::vec3(0.0f, -5.0f, 0.0f));
	narrow.SetTarPos(glm::vec3(0.0f));

	EditorViewport a;
	a.SetViewportSize(glm::uvec2(1600u, 600u), glm::uvec2(1600u, 600u));
	a.SetCamera(&wide);

	EditorViewport b;
	b.SetViewportSize(glm::uvec2(400u, 600u), glm::uvec2(400u, 600u));
	b.SetCamera(&narrow);

	EXPECT_NEAR(PixelsPerWorldUnit(a, 5.0f), PixelsPerWorldUnit(b, 5.0f), 1e-3f);
}

/// @test Twice the depth is half the pixels — the perspective divide, nothing more.
TEST_F(EditorViewportTest, PixelsPerWorldUnit_HalvesAtDoubleDepth)
{
	const float near5 = PixelsPerWorldUnit(m_vp, 5.0f);
	const float far10 = PixelsPerWorldUnit(m_vp, 10.0f);
	ASSERT_GT(near5, 0.0f);
	EXPECT_NEAR(far10 * 2.0f, near5, 1e-3f);
}

/**
 * @test It agrees with what Project() actually measures.
 *
 * The closed form is derived from proj[1][1], so this is the one assertion that
 * ties it back to the projection it claims to describe.
 */
TEST_F(EditorViewportTest, PixelsPerWorldUnit_MatchesAProjectedUnitOffset)
{
	const ScreenPoint origin = m_vp.Project(glm::vec3(0.0f));
	const ScreenPoint up     = m_vp.Project(glm::vec3(0.0f, 0.0f, 1.0f));
	ASSERT_TRUE(origin.visible && up.visible);

	const float measured = std::abs(up.pixel.y - origin.pixel.y);
	EXPECT_NEAR(measured, PixelsPerWorldUnit(m_vp, origin.viewDepth), 1e-2f);
}

/// @test An invalid viewport reports zero rather than a division by a zero height.
TEST(EditorViewport, PixelsPerWorldUnit_ZeroWhenInvalid)
{
	EditorViewport vp;
	EXPECT_FLOAT_EQ(PixelsPerWorldUnit(vp, 5.0f), 0.0f);
}

// ===========================================================================
// E. ProjectBox
// ===========================================================================

/// @test A box fully in front projects to a rect around the centre.
TEST_F(EditorViewportTest, ProjectBox_EnclosesTheProjectedCentre)
{
	const ScreenRect r = m_vp.ProjectBox(glm::mat4(1.0f),
	                                     glm::vec3(-1.0f), glm::vec3(1.0f));
	ASSERT_TRUE(r.valid);
	EXPECT_FALSE(r.clamped);

	const ScreenPoint c = m_vp.Project(glm::vec3(0.0f));
	EXPECT_LT(r.min.x, c.pixel.x);
	EXPECT_GT(r.max.x, c.pixel.x);
	EXPECT_LT(r.min.y, c.pixel.y);
	EXPECT_GT(r.max.y, c.pixel.y);
}

/**
 * @test A box straddling the near plane gets *wider*, never a small mirrored rect.
 *
 * This is the failure mode that looks plausible: divide a corner behind the eye
 * by its negative w and it lands on the far side of the viewport, so the rect
 * comes out finite, small and completely wrong. Clipping the edges at the near
 * plane instead makes the box cover the screen, which is the truth.
 */
TEST_F(EditorViewportTest, ProjectBox_StraddlingClipsAtTheNearPlaneInsteadOfWrapping)
{
	const ScreenRect ahead = m_vp.ProjectBox(glm::mat4(1.0f),
	                                         glm::vec3(-1.0f), glm::vec3(1.0f));
	ASSERT_TRUE(ahead.valid);

	// Centred on the eye: half its corners are behind the near plane.
	const glm::mat4 onEye = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -5.0f, 0.0f));
	const ScreenRect straddling = m_vp.ProjectBox(onEye, glm::vec3(-1.0f), glm::vec3(1.0f));

	ASSERT_TRUE(straddling.valid) << "partly-visible is visible, not invalid";
	EXPECT_GT(straddling.max.x - straddling.min.x, ahead.max.x - ahead.min.x);
	EXPECT_LT(straddling.min.x, 0.0f) << "it must spill off-screen, not fold inward";
	EXPECT_GT(straddling.max.x, static_cast<float>(kW));
}

/// @test A box entirely behind the eye projects to nothing at all.
TEST_F(EditorViewportTest, ProjectBox_WhollyBehindIsInvalid)
{
	const glm::mat4 behind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -20.0f, 0.0f));
	EXPECT_FALSE(m_vp.ProjectBox(behind, glm::vec3(-1.0f), glm::vec3(1.0f)).valid);
}

/// @test ProjectBounds is ProjectBox with an identity model.
TEST_F(EditorViewportTest, ProjectBounds_MatchesAnIdentityProjectBox)
{
	const ScreenRect direct = m_vp.ProjectBox(glm::mat4(1.0f),
	                                          glm::vec3(-1.0f), glm::vec3(2.0f));
	const ScreenRect wrapped = ProjectBounds(m_vp, glm::vec3(-1.0f), glm::vec3(2.0f));
	ASSERT_TRUE(direct.valid && wrapped.valid);
	EXPECT_NEAR(direct.min.x, wrapped.min.x, 1e-3f);
	EXPECT_NEAR(direct.max.y, wrapped.max.y, 1e-3f);
}

// ===========================================================================
// F. ViewAxis — the shared derivation
// ===========================================================================

/// @test It is the unit direction from the eye to the target.
TEST_F(EditorViewportTest, ViewAxis_PointsFromEyeToTarget)
{
	const glm::vec3 axis = m_vp.ViewAxis();
	EXPECT_NEAR(glm::length(axis), 1.0f, 1e-5f);
	EXPECT_NEAR(axis.x, 0.0f, 1e-5f);
	EXPECT_NEAR(axis.y, 1.0f, 1e-5f);
	EXPECT_NEAR(axis.z, 0.0f, 1e-5f);
}

/**
 * @test No camera, or one looking at its own eye, yields the zero vector.
 *
 * Returning zero rather than normalizing a zero-length vector is what keeps a
 * NaN out of the free Move's plane solve and the free Rotate's turn axis. Every
 * caller tests the length; none re-derives the camera terms.
 */
TEST(EditorViewport, ViewAxis_ZeroWhenThereIsNoDirection)
{
	EditorViewport vp;
	EXPECT_FLOAT_EQ(glm::length(vp.ViewAxis()), 0.0f) << "no camera";

	Camera degenerate;
	degenerate.SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	degenerate.SetTarPos(glm::vec3(1.0f, 2.0f, 3.0f));
	vp.SetViewportSize(glm::uvec2(kW, kH), glm::uvec2(kW, kH));
	vp.SetCamera(&degenerate);
	EXPECT_FLOAT_EQ(glm::length(vp.ViewAxis()), 0.0f) << "target sits on the eye";
}

// ===========================================================================
// G. DPI and object queries
// ===========================================================================

/// @test ToRenderPixels is the one deliberate exit into physical pixels.
TEST_F(EditorViewportTest, ToRenderPixels_ScalesByTheDeviceRatio)
{
	m_vp.SetViewportSize(glm::uvec2(kW, kH), glm::uvec2(kW * 2u, kH * 2u));
	EXPECT_FLOAT_EQ(m_vp.DeviceRatio(), 2.0f);

	const glm::vec2 physical = m_vp.ToRenderPixels(glm::vec2(100.0f, 50.0f));
	EXPECT_FLOAT_EQ(physical.x, 200.0f);
	EXPECT_FLOAT_EQ(physical.y, 100.0f);
}

/// @test A logical-only viewport reports ratio 1 and passes pixels straight through.
TEST_F(EditorViewportTest, ToRenderPixels_IdentityWithoutHiDPI)
{
	EXPECT_FLOAT_EQ(m_vp.DeviceRatio(), 1.0f);
	const glm::vec2 same = m_vp.ToRenderPixels(glm::vec2(7.0f, 11.0f));
	EXPECT_FLOAT_EQ(same.x, 7.0f);
	EXPECT_FLOAT_EQ(same.y, 11.0f);
}

/// @test ScreenPosition agrees with Project() on the object's own position.
TEST_F(EditorViewportTest, ScreenPosition_MatchesProjectOfTheTransform)
{
	Camera subject;
	subject.SetPosition(glm::vec3(1.0f, 0.0f, 2.0f));

	const ScreenPoint viaObject = ScreenPosition(m_vp, subject);
	const ScreenPoint viaPoint  = m_vp.Project(glm::vec3(1.0f, 0.0f, 2.0f));
	ASSERT_TRUE(viaObject.visible && viaPoint.visible);
	EXPECT_NEAR(viaObject.pixel.x, viaPoint.pixel.x, 1e-3f);
	EXPECT_NEAR(viaObject.pixel.y, viaPoint.pixel.y, 1e-3f);
}

/**
 * @test An object with no Transform3D is invisible, not at the origin.
 *
 * Sprite is screen-space and derives from ObjectID alone. Guessing an origin
 * would let a caller silently treat "has no position" as "is at (0,0,0)".
 */
TEST_F(EditorViewportTest, ScreenPosition_InvisibleWithoutATransform)
{
	Sprite sprite;
	EXPECT_FALSE(ScreenPosition(m_vp, sprite).visible);
}
