/**
 * @file test_camera.cpp
 * @brief Unit tests for Camera scene object.
 *
 * TDD: RED (test written first) -> GREEN (implementation verified).
 * All tests are pure CPU math -- no GPU required.
 */

#include <gtest/gtest.h>

#include "scene/Camera.hpp"

using namespace neurus;

// -----------------------------------------------------------------------
// Default construction
// -----------------------------------------------------------------------

/**
 * @test Default-constructed Camera has expected default parameters.
 */
TEST(Camera, DefaultConstruction)
{
	Camera cam;

	EXPECT_FLOAT_EQ(cam.cam_pers, 60.0f);
	EXPECT_FLOAT_EQ(cam.cam_near, 0.1f);
	EXPECT_FLOAT_EQ(cam.cam_far, 100.0f);
	EXPECT_FLOAT_EQ(cam.cam_w, 1.0f);
	EXPECT_FLOAT_EQ(cam.cam_h, 1.0f);
	EXPECT_EQ(cam.cam_tar, glm::vec3(0.0f, 0.0f, 0.0f));
}

/**
 * @test Camera ObjectID type is set to GO_CAM.
 */
TEST(Camera, ObjectTypeIsGOCAM)
{
	Camera cam;
	EXPECT_EQ(cam.o_type, ObjectID::GOType::GO_CAM);
}

/**
 * @test Camera inherits a valid UID.
 */
TEST(Camera, InheritsValidUID)
{
	Camera cam;
	EXPECT_GE(cam.GetObjectID(), 0);
}

// -----------------------------------------------------------------------
// Non-copyable
// -----------------------------------------------------------------------

/**
 * @test Camera is non-copyable (RAII semantics).
 */
TEST(Camera, NonCopyable)
{
	EXPECT_FALSE(std::is_copy_constructible_v<Camera>);
	EXPECT_FALSE(std::is_copy_assignable_v<Camera>);
}

// -----------------------------------------------------------------------
// View matrix
// -----------------------------------------------------------------------

/**
 * @test GetViewMatrix changes with camera position.
 *
 * Camera at (0, -5, 0) looking at origin should produce a valid
 * view matrix where the translation component reflects the position.
 * (Z-up convention: X=right, Y=forward, Z=up)
 */
TEST(Camera, ViewMatrix_ChangesWithPosition)
{
	Camera cam;
	cam.SetPosition(glm::vec3(0.0f, -5.0f, 0.0f));

	glm::mat4 view = cam.GetViewMatrix();

	// For camera at (0,-5,0) looking at (0,0,0) with glm::lookAt(eye, target, up=(0,0,1)):
	// f = normalize(center - eye) = (0, 1, 0)
	// s = normalize(f x up) = (1, 0, 0)
	// u = s x f = (0, 0, 1)
	// view[3] = (-dot(s,eye), -dot(u,eye), dot(f,eye), 1) = (0, 0, -5, 1)
	// view[2] = (s.z, u.z, -f.z, dot(f,eye)) = (0, 1, 0, -5)
	EXPECT_NEAR(view[3][0], 0.0f, 1e-5f);
	EXPECT_NEAR(view[3][1], 0.0f, 1e-5f);
	EXPECT_NEAR(view[3][2], -5.0f, 1e-5f);

	EXPECT_NEAR(view[2][0], 0.0f, 1e-5f);
	EXPECT_NEAR(view[2][1], 1.0f, 1e-5f);
	EXPECT_NEAR(view[2][2], 0.0f, 1e-5f);
}

/**
 * @test GetViewMatrix changes with look-at target.
 */
TEST(Camera, ViewMatrix_ChangesWithTarget)
{
	Camera cam;
	cam.SetPosition(glm::vec3(0.0f, -5.0f, 0.0f));

	glm::mat4 viewAtOrigin = cam.GetViewMatrix();

	// Change target to (0, 0, 5) -- camera now looks up (Z-up)
	cam.SetTarPos(glm::vec3(0.0f, 0.0f, 5.0f));
	glm::mat4 viewAtUp = cam.GetViewMatrix();

	// Matrices must differ
	EXPECT_NE(viewAtOrigin, viewAtUp);
}

/**
 * @test Two cameras with same position and target produce same view matrix.
 */
TEST(Camera, ViewMatrix_Deterministic)
{
	Camera camA;
	Camera camB;

	camA.SetPosition(glm::vec3(10.0f, -3.0f, 5.0f));
	camA.SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	camB.SetPosition(glm::vec3(10.0f, -3.0f, 5.0f));
	camB.SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	EXPECT_EQ(camA.GetViewMatrix(), camB.GetViewMatrix());
}

// -----------------------------------------------------------------------
// Projection matrix
// -----------------------------------------------------------------------

/**
 * @test GetProjectionMatrix returns a valid perspective projection.
 *
 * With 60° FOV, 1.0 aspect, near=0.1, far=100:
 * - M[0][0] = 1 / tan(30°) / aspect = 1.73205
 * - M[1][1] = 1 / tan(30°) = 1.73205
 * - M[2][3] = -1 (perspective divide)
 * - M[3][3] = 0
 */
TEST(Camera, ProjectionMatrix_CorrectStructure)
{
	Camera cam;

	glm::mat4 proj = cam.GetProjectionMatrix();

	// Expected: f = 1 / tan(30°) = 1.7320508; Y negated for Vulkan NDC
	const float expectedF = 1.0f / std::tan(glm::radians(30.0f));

	EXPECT_NEAR(proj[0][0], expectedF, 1e-5f);
	EXPECT_NEAR(proj[1][1], -expectedF, 1e-5f);   // negated for Vulkan Y-flip
	EXPECT_FLOAT_EQ(proj[2][3], -1.0f);
	EXPECT_FLOAT_EQ(proj[3][3], 0.0f);

	// Near/far entries: both negative in right-handed GLM perspective
	EXPECT_LT(proj[2][2], 0.0f);
	EXPECT_LT(proj[3][2], 0.0f);
}

/**
 * @test Changing aspect ratio produces a different projection matrix.
 */
TEST(Camera, ProjectionMatrix_AspectChanges)
{
	Camera cam;
	cam.SetPosition(glm::vec3(0.0f, 0.0f, 5.0f));

	glm::mat4 squareProj = cam.GetProjectionMatrix();

	// Change aspect to 16:9
	cam.ChangeCamRatio(16.0f, 9.0f);
	glm::mat4 wideProj = cam.GetProjectionMatrix();

	EXPECT_NE(squareProj, wideProj);
	// Wide aspect should have smaller M[0][0] (f/aspect)
	EXPECT_LT(wideProj[0][0], squareProj[0][0]);
}

/**
 * @test Changing FOV produces a different projection matrix.
 */
TEST(Camera, ProjectionMatrix_FOVChanges)
{
	Camera cam;

	glm::mat4 proj60 = cam.GetProjectionMatrix();

	cam.ChangeCamPersp(90.0f);
	glm::mat4 proj90 = cam.GetProjectionMatrix();

	EXPECT_NE(proj60, proj90);
}

// -----------------------------------------------------------------------
// Projection caching / dirty flag
// -----------------------------------------------------------------------

/**
 * @test GetProjectionMatrix returns cached matrix when no parameters change.
 */
TEST(Camera, ProjectionMatrix_Caching)
{
	Camera cam;

	glm::mat4 first = cam.GetProjectionMatrix();
	glm::mat4 second = cam.GetProjectionMatrix();

	EXPECT_EQ(first, second);
}

/**
 * @test GetProjectionMatrix recomputes after ChangeCamRatio.
 */
TEST(Camera, ProjectionMatrix_InvalidatedByRatio)
{
	Camera cam;
	cam.GetProjectionMatrix(); // populate cache

	cam.ChangeCamRatio(800.0f, 600.0f);
	glm::mat4 recomputed = cam.GetProjectionMatrix();

	// Verify new aspect produces different M[0][0]
	const float aspect = 800.0f / 600.0f;
	const float expectedFovScale = 1.0f / std::tan(glm::radians(30.0f));
	EXPECT_NEAR(recomputed[0][0], expectedFovScale / aspect, 1e-5f);
}

/**
 * @test GetProjectionMatrix recomputes after ChangeCamPersp.
 */
TEST(Camera, ProjectionMatrix_InvalidatedByFOV)
{
	Camera cam;
	cam.GetProjectionMatrix(); // populate cache

	cam.ChangeCamPersp(90.0f);
	glm::mat4 recomputed = cam.GetProjectionMatrix();

	// For 90° FOV: f = 1 / tan(45°) = 1.0; Y negated for Vulkan NDC
	const float expectedF = 1.0f / std::tan(glm::radians(45.0f));
	EXPECT_NEAR(recomputed[0][0], expectedF, 1e-5f);
	EXPECT_NEAR(recomputed[1][1], -expectedF, 1e-5f);   // negated for Vulkan Y-flip
}

// -----------------------------------------------------------------------
// ChangeCamRatio
// -----------------------------------------------------------------------

/**
 * @test ChangeCamRatio updates cam_w and cam_h.
 */
TEST(Camera, ChangeCamRatio_UpdatesDimensions)
{
	Camera cam;
	cam.ChangeCamRatio(1920.0f, 1080.0f);

	EXPECT_FLOAT_EQ(cam.cam_w, 1920.0f);
	EXPECT_FLOAT_EQ(cam.cam_h, 1080.0f);
}

// -----------------------------------------------------------------------
// ChangeCamPersp
// -----------------------------------------------------------------------

/**
 * @test ChangeCamPersp updates cam_pers.
 */
TEST(Camera, ChangeCamPersp_UpdatesFOV)
{
	Camera cam;
	cam.ChangeCamPersp(90.0f);

	EXPECT_FLOAT_EQ(cam.cam_pers, 90.0f);
}

// -----------------------------------------------------------------------
// SetPosition / SetTarPos
// -----------------------------------------------------------------------

/**
 * @test SetPosition updates the Transform3D position.
 */
TEST(Camera, SetPosition_UpdatesTransform)
{
	Camera cam;
	cam.SetPosition(glm::vec3(10.0f, 20.0f, 30.0f));

	EXPECT_EQ(cam.GetPosition(), glm::vec3(10.0f, 20.0f, 30.0f));
}

/**
 * @test SetTarPos updates the look-at target.
 */
TEST(Camera, SetTarPos_UpdatesTarget)
{
	Camera cam;
	cam.SetTarPos(glm::vec3(1.0f, 2.0f, 3.0f));

	EXPECT_EQ(cam.cam_tar, glm::vec3(1.0f, 2.0f, 3.0f));
}

// -----------------------------------------------------------------------
// GenFloatData
// -----------------------------------------------------------------------

/**
 * @test GenFloatData produces 8 floats.
 */
TEST(Camera, GenFloatData_Size)
{
	Camera cam;
	cam.SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	cam.SetRotation(glm::vec3(10.0f, 20.0f, 30.0f));
	cam.ChangeCamRatio(16.0f, 9.0f);
	cam.ChangeCamPersp(45.0f);

	cam.GenFloatData();

	ASSERT_EQ(cam.cam_floatData.size(), 8U);

	// Position
	EXPECT_FLOAT_EQ(cam.cam_floatData[0], 1.0f);
	EXPECT_FLOAT_EQ(cam.cam_floatData[1], 2.0f);
	EXPECT_FLOAT_EQ(cam.cam_floatData[2], 3.0f);

	// Rotation (degrees)
	EXPECT_FLOAT_EQ(cam.cam_floatData[3], 10.0f);
	EXPECT_FLOAT_EQ(cam.cam_floatData[4], 20.0f);
	EXPECT_FLOAT_EQ(cam.cam_floatData[5], 30.0f);

	// Aspect ratio
	EXPECT_FLOAT_EQ(cam.cam_floatData[6], 16.0f / 9.0f);

	// FOV in radians
	EXPECT_FLOAT_EQ(cam.cam_floatData[7], glm::radians(45.0f));
}

/**
 * @test GenFloatData can be called multiple times (idempotent).
 */
TEST(Camera, GenFloatData_Idempotent)
{
	Camera cam;
	cam.SetPosition(glm::vec3(5.0f, 5.0f, 5.0f));

	cam.GenFloatData();
	std::vector<float> first = cam.cam_floatData;

	cam.GenFloatData();
	ASSERT_EQ(cam.cam_floatData.size(), 8U);

	for (int i = 0; i < 8; ++i)
	{
		EXPECT_FLOAT_EQ(cam.cam_floatData[i], first[i]);
	}
}

/**
 * @test GenFloatData defaults when no parameters changed.
 */
TEST(Camera, GenFloatData_Defaults)
{
	Camera cam;
	cam.GenFloatData();

	ASSERT_EQ(cam.cam_floatData.size(), 8U);

	// Default position = (0, 0, 0)
	EXPECT_FLOAT_EQ(cam.cam_floatData[0], 0.0f);
	EXPECT_FLOAT_EQ(cam.cam_floatData[1], 0.0f);
	EXPECT_FLOAT_EQ(cam.cam_floatData[2], 0.0f);

	// Default rotation = (0, 0, 0)
	EXPECT_FLOAT_EQ(cam.cam_floatData[3], 0.0f);
	EXPECT_FLOAT_EQ(cam.cam_floatData[4], 0.0f);
	EXPECT_FLOAT_EQ(cam.cam_floatData[5], 0.0f);

	// Default aspect = 1.0
	EXPECT_FLOAT_EQ(cam.cam_floatData[6], 1.0f);

	// Default FOV = 60 deg = PI/3 rad
	EXPECT_FLOAT_EQ(cam.cam_floatData[7], glm::radians(60.0f));
}

// -----------------------------------------------------------------------
// GetTransform
// -----------------------------------------------------------------------

/**
 * @test GetTransform returns a non-null void pointer.
 */
TEST(Camera, GetTransform_ReturnsNonNull)
{
	Camera cam;
	EXPECT_NE(cam.GetTransform(), nullptr);
}

/**
 * @test GetTransform pointer can be cast to Transform* and is valid.
 */
TEST(Camera, GetTransform_IsTransform)
{
	Camera cam;
	void* ptr = cam.GetTransform();

	// Must be convertible to Transform*
	Transform* trans = static_cast<Transform*>(ptr);
	EXPECT_NE(trans, nullptr);

	// GetTransformPtr on it should return non-null
	EXPECT_NE(trans->GetTransformPtr(), nullptr);
}

/**
 * @test GetTransform returns a pointer to this Camera's Transform3D.
 */
TEST(Camera, GetTransform_PointsToSelf)
{
	Camera cam;
	cam.SetPosition(glm::vec3(42.0f, 0.0f, 0.0f));

	void* ptr = cam.GetTransform();
	Transform* trans = static_cast<Transform*>(ptr);
	Transform3D* t3d = static_cast<Transform3D*>(trans);

	// If it points to the Camera's Transform3D, position matches
	EXPECT_EQ(t3d->GetPosition(), glm::vec3(42.0f, 0.0f, 0.0f));
}

// -----------------------------------------------------------------------
// Explicit parameter constructor
// -----------------------------------------------------------------------

/**
 * @test Camera can be constructed with explicit parameters.
 */
TEST(Camera, ExplicitConstruction)
{
	Camera cam(1920.0f, 1080.0f, 90.0f, 0.01f, 500.0f);

	EXPECT_FLOAT_EQ(cam.cam_w, 1920.0f);
	EXPECT_FLOAT_EQ(cam.cam_h, 1080.0f);
	EXPECT_FLOAT_EQ(cam.cam_pers, 90.0f);
	EXPECT_FLOAT_EQ(cam.cam_near, 0.01f);
	EXPECT_FLOAT_EQ(cam.cam_far, 500.0f);
	EXPECT_EQ(cam.o_type, ObjectID::GOType::GO_CAM);
}

// -----------------------------------------------------------------------
// Edge cases
// -----------------------------------------------------------------------

/**
 * @test Camera with extreme aspect ratio still produces a valid projection.
 */
TEST(Camera, ExtremeAspectRatio)
{
	Camera cam;
	cam.ChangeCamRatio(10000.0f, 1.0f);

	glm::mat4 proj = cam.GetProjectionMatrix();

	// Should not produce NaN or Inf
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			EXPECT_FALSE(std::isnan(proj[col][row]));
			EXPECT_FALSE(std::isinf(proj[col][row]));
		}
	}

	// Very wide aspect: M[0][0] should be very small
	EXPECT_GT(proj[0][0], 0.0f);
	EXPECT_LT(proj[0][0], 0.01f);
}
