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

TEST(Camera, DefaultConstruction)
{
	Camera cam;

	EXPECT_FLOAT_EQ(cam.cam_pers, 60.0f);
	EXPECT_FLOAT_EQ(cam.cam_near, 0.1f);
	EXPECT_FLOAT_EQ(cam.cam_far, 100.0f);
	EXPECT_FLOAT_EQ(cam.cam_w, 1.0f);
	EXPECT_FLOAT_EQ(cam.cam_h, 1.0f);
	EXPECT_EQ(cam.cam_tar, glm::vec3(0.0f, 0.0f, 0.0f));

	EXPECT_EQ(cam.o_type, ObjectID::GOType::GO_CAM);
	EXPECT_EQ(cam.GetTransform(), static_cast<Transform3D*>(&cam));
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
TEST(Camera, ViewMatrixTracksEyeAndTarget)
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
	cam.SetTarPos(glm::vec3(0.0f, 0.0f, 5.0f));
	EXPECT_NE(cam.GetViewMatrix(), view);
}

// -----------------------------------------------------------------------
// Projection matrix
// -----------------------------------------------------------------------

/**
 * @test GetProjectionMatrix returns a valid perspective projection.
 *
 * With 60° FOV and 1.0 aspect:
 * - M[0][0] = 1 / tan(30°) / aspect = 1.73205
 * - M[1][1] = -1 / tan(30°) = -1.73205 (Vulkan Y flip)
 * - M[2][3] = -1 (perspective divide)
 * - M[3][3] = 0
 */
TEST(Camera, ProjectionTracksAspectAndFieldOfView)
{
	Camera cam;
	struct Lens { float width, height, fov; };
	for (const Lens lens : {Lens{1, 1, 60}, Lens{800, 600, 60},
	                        Lens{800, 600, 90}, Lens{10000, 1, 90}})
	{
		SCOPED_TRACE(::testing::Message() << lens.width << "x" << lens.height << " FOV " << lens.fov);
		// Keep each setter independent so calling the other cannot hide a stale cache.
		if (cam.cam_w != lens.width || cam.cam_h != lens.height)
			cam.ChangeCamRatio(lens.width, lens.height);
		if (cam.cam_pers != lens.fov)
			cam.ChangeCamPersp(lens.fov);
		const auto projection = cam.GetProjectionMatrix();
		const float scale = 1.0f / std::tan(glm::radians(lens.fov * 0.5f));
		EXPECT_NEAR(projection[0][0], scale * lens.height / lens.width, 1e-6f);
		EXPECT_NEAR(projection[1][1], -scale, 1e-5f);
		EXPECT_EQ(projection[2][3], -1.0f);
		EXPECT_EQ(projection[3][3], 0.0f);
		EXPECT_LT(projection[2][2], 0.0f);
		EXPECT_LT(projection[3][2], 0.0f);
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
				EXPECT_TRUE(std::isfinite(projection[col][row]));
	}
}

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
	const auto packed = cam.cam_floatData;
	cam.GenFloatData();
	EXPECT_EQ(cam.cam_floatData, packed);
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
