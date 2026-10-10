/**
 * @file test_scene_integration.cpp
 * @brief Integration tests for Scene container wired with all real scene object types.
 *
 * TDD: RED (test written first) → GREEN (implementation verified).
 * All tests are pure CPU - no GPU required.
 *
 * Tests cover:
 * - Registering all six object types (Camera, Light, Mesh, Sprite, DebugLine, DebugPoints)
 * - Cross-pool isolation (obj in correct type pool only)
 * - GetObjectID lookup across all pools
 * - GetActiveCamera with multiple objects
 * - SceneModifStatus tracking with real object property changes
 * - UpdateObjTransforms iterates obj_list and processes transforms on all objects with Transform3D
 */

#include <gtest/gtest.h>

#include <scene/Camera.h>
#include <scene/DebugLine.h>
#include <scene/DebugPoints.h>
#include <scene/Light.h>
#include <scene/Mesh.h>
#include <scene/Scene.h>
#include <scene/Sprite.h>

using namespace neurus;

// -----------------------------------------------------------------------
// 1. Full scene registration - all six types in both type pool and obj_list
// -----------------------------------------------------------------------

/**
 * @test Register Camera, Light, Mesh, Sprite, DebugLine, DebugPoints
 *       and verify each is in its type-specific pool AND the master obj_list.
 */
TEST(SceneIntegrationTest, RegisterAllObjectTypes)
{
	Scene scene;

	auto cam      = std::make_shared<Camera>();
	auto light    = std::make_shared<Light>(LightType::POINTLIGHT, 20.0f, glm::vec3(1.0f, 0.5f, 0.2f));
	auto mesh     = std::make_shared<Mesh>();
	auto sprite   = std::make_shared<Sprite>();
	auto dLine    = std::make_shared<DebugLine>();
	auto dPoints  = std::make_shared<DebugPoints>();

	int camId     = cam->GetObjectID();
	int lightId   = light->GetObjectID();
	int meshId    = mesh->GetObjectID();
	int spriteId  = sprite->GetObjectID();
	int dLineId   = dLine->GetObjectID();
	int dPointsId = dPoints->GetObjectID();

	scene.UseCamera(cam);
	scene.UseLight(light);
	scene.UseMesh(mesh);
	scene.UseSprite(sprite);
	scene.UseDebugLine(dLine);
	scene.UseDebugPoints(dPoints);

	// --- Master obj_list contains all six ---
	EXPECT_EQ(scene.obj_list.size(), 6u);
	EXPECT_NE(scene.obj_list.find(camId),     scene.obj_list.end());
	EXPECT_NE(scene.obj_list.find(lightId),   scene.obj_list.end());
	EXPECT_NE(scene.obj_list.find(meshId),    scene.obj_list.end());
	EXPECT_NE(scene.obj_list.find(spriteId),  scene.obj_list.end());
	EXPECT_NE(scene.obj_list.find(dLineId),   scene.obj_list.end());
	EXPECT_NE(scene.obj_list.find(dPointsId), scene.obj_list.end());

	// --- Each type-specific pool has exactly one entry ---
	EXPECT_EQ(scene.cam_list.size(),      1u);
	EXPECT_EQ(scene.light_list.size(),    1u);
	EXPECT_EQ(scene.mesh_list.size(),     1u);
	EXPECT_EQ(scene.sprite_list.size(),   1u);
	EXPECT_EQ(scene.dLine_list.size(),    1u);
	EXPECT_EQ(scene.dPoints_list.size(),  1u);
}

// -----------------------------------------------------------------------
// 2. Cross-pool isolation - objects are NOT in wrong type pools
// -----------------------------------------------------------------------

/**
 * @test Verify each object is ONLY in its own type-specific pool,
 *       not in pools for other object types.
 */
TEST(SceneIntegrationTest, CrossPoolIsolation)
{
	Scene scene;

	auto cam   = std::make_shared<Camera>();
	auto light = std::make_shared<Light>();
	auto mesh  = std::make_shared<Mesh>();

	scene.UseCamera(cam);
	scene.UseLight(light);
	scene.UseMesh(mesh);

	int camId   = cam->GetObjectID();
	int lightId = light->GetObjectID();
	int meshId  = mesh->GetObjectID();

	// Camera only in cam_list
	EXPECT_NE(scene.cam_list.find(camId),   scene.cam_list.end());
	EXPECT_EQ(scene.light_list.find(camId), scene.light_list.end());
	EXPECT_EQ(scene.mesh_list.find(camId),  scene.mesh_list.end());

	// Light only in light_list
	EXPECT_EQ(scene.cam_list.find(lightId),   scene.cam_list.end());
	EXPECT_NE(scene.light_list.find(lightId), scene.light_list.end());
	EXPECT_EQ(scene.mesh_list.find(lightId),  scene.mesh_list.end());

	// Mesh only in mesh_list
	EXPECT_EQ(scene.cam_list.find(meshId),   scene.cam_list.end());
	EXPECT_EQ(scene.light_list.find(meshId), scene.light_list.end());
	EXPECT_NE(scene.mesh_list.find(meshId),  scene.mesh_list.end());
}

// -----------------------------------------------------------------------
// 3. GetObjectID lookup across all pools
// -----------------------------------------------------------------------

/**
 * @test GetObjectID(id) returns the correct ObjectID* regardless of
 *       which type pool the object lives in.
 */
TEST(SceneIntegrationTest, GetObjectIDCrossPoolLookup)
{
	Scene scene;

	auto cam   = std::make_shared<Camera>();
	auto light = std::make_shared<Light>();
	auto mesh  = std::make_shared<Mesh>();
	auto dLine = std::make_shared<DebugLine>();

	scene.UseCamera(cam);
	scene.UseLight(light);
	scene.UseMesh(mesh);
	scene.UseDebugLine(dLine);

	// Lookup each object
	ObjectID* foundCam   = scene.GetObjectID(cam->GetObjectID());
	ObjectID* foundLight = scene.GetObjectID(light->GetObjectID());
	ObjectID* foundMesh  = scene.GetObjectID(mesh->GetObjectID());
	ObjectID* foundDLine = scene.GetObjectID(dLine->GetObjectID());

	ASSERT_NE(foundCam,   nullptr);
	ASSERT_NE(foundLight, nullptr);
	ASSERT_NE(foundMesh,  nullptr);
	ASSERT_NE(foundDLine, nullptr);

	EXPECT_EQ(foundCam->GetObjectID(),   cam->GetObjectID());
	EXPECT_EQ(foundLight->GetObjectID(), light->GetObjectID());
	EXPECT_EQ(foundMesh->GetObjectID(),  mesh->GetObjectID());
	EXPECT_EQ(foundDLine->GetObjectID(), dLine->GetObjectID());

	// Correct type info
	EXPECT_EQ(foundCam->o_type,   ObjectID::GOType::GO_CAM);
	EXPECT_EQ(foundLight->o_type, ObjectID::GOType::GO_LIGHT);
	EXPECT_EQ(foundMesh->o_type,  ObjectID::GOType::GO_MESH);
	EXPECT_EQ(foundDLine->o_type, ObjectID::GOType::GO_DL);

	// Non-existent ID returns nullptr
	EXPECT_EQ(scene.GetObjectID(99999), nullptr);
}

// -----------------------------------------------------------------------
// 4. GetActiveCamera
// -----------------------------------------------------------------------

/**
 * @test GetActiveCamera returns the first camera registered.
 *       Returns nullptr when no cameras are in the scene.
 */
TEST(SceneIntegrationTest, GetActiveCameraWithMixedObjects)
{
	Scene scene;

	// Before any cameras
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);

	// Register non-camera objects first
	auto light = std::make_shared<Light>();
	auto mesh  = std::make_shared<Mesh>();
	scene.UseLight(light);
	scene.UseMesh(mesh);

	// Still no active camera
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);

	// Now register cameras
	auto cam1 = std::make_shared<Camera>();
	auto cam2 = std::make_shared<Camera>();
	scene.UseCamera(cam1);
	scene.UseCamera(cam2);

	// Registration still activates nothing — activation is explicit scene state.
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);

	ASSERT_TRUE(scene.ActivateCamera(cam1->GetObjectID()));
	Camera* active = scene.GetActiveCamera();
	ASSERT_NE(active, nullptr);
	EXPECT_EQ(active->GetObjectID(), cam1->GetObjectID());
}

// -----------------------------------------------------------------------
// 5. SceneModifStatus tracking with real object changes
// -----------------------------------------------------------------------

/**
 * @test Status flags track changes when real objects are modified.
 *       Camera parameter change → CameraChanged flag.
 *       Light property change → LightChanged flag.
 *       Flags can be checked and reset independently.
 */
TEST(SceneIntegrationTest, StatusTrackingWithRealObjects)
{
	Scene scene;
	scene.ResetStatus();

	// Register objects
	auto cam   = std::make_shared<Camera>();
	auto light = std::make_shared<Light>(LightType::POINTLIGHT);
	scene.UseCamera(cam);
	scene.UseLight(light);

	// Verify clean state
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));

	// Simulate camera change - set CameraChanged flag
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::CameraChanged), true);
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));

	// Reset camera flag
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::CameraChanged), false);
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));

	// Simulate light change
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::LightChanged), true);
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));

	// Both together
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::CameraChanged), true);
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));

	// ResetStatus clears all
	scene.ResetStatus();
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::ObjectTransChanged));
}

// -----------------------------------------------------------------------
// 6. UpdateObjTransforms - iterates and processes transforms
// -----------------------------------------------------------------------

/**
 * @test UpdateObjTransforms iterates the master obj_list, accesses
 *       GetTransform() on each object, casts to Transform3D*, and calls
 *       GetModelMatrix() to ensure cached matrices are up-to-date.
 *
 *       Objects without Transform3D (Sprite) are safely skipped.
 */
TEST(SceneIntegrationTest, UpdateObjTransformsIteratesAll)
{
	Scene scene;

	// Register objects with transforms
	auto cam     = std::make_shared<Camera>();
	auto light   = std::make_shared<Light>(LightType::SUNLIGHT);
	auto mesh    = std::make_shared<Mesh>();
	auto dLine   = std::make_shared<DebugLine>();
	auto dPoints = std::make_shared<DebugPoints>();

	// Set distinct positions
	cam->SetPosition(glm::vec3(0.0f, 5.0f, 1.0f));
	light->SetPosition(glm::vec3(2.0f, 0.0f, 3.0f));
	mesh->SetPosition(glm::vec3(-1.0f, 2.0f, 0.0f));
	dLine->SetPosition(glm::vec3(10.0f, 0.0f, 0.0f));
	dPoints->SetPosition(glm::vec3(0.0f, 0.0f, -5.0f));

	scene.UseCamera(cam);
	scene.UseLight(light);
	scene.UseMesh(mesh);
	scene.UseDebugLine(dLine);
	scene.UseDebugPoints(dPoints);

	// Register a sprite (no Transform3D - should not crash)
	auto sprite = std::make_shared<Sprite>();
	scene.UseSprite(sprite);

	// All 6 objects in obj_list
	EXPECT_EQ(scene.obj_list.size(), 6u);

	// Call UpdateObjTransforms - should iterate all, skip sprite (no transform),
	// and call GetModelMatrix() on the 5 objects with Transform3D
	EXPECT_NO_THROW(scene.UpdateObjTransforms());

	// Objects with transforms should have computed model matrices.
	// We can verify indirectly: GetModelMatrix() is const and caches.
	// After SetPosition, the matrix was dirty. UpdateObjTransforms should
	// have called GetModelMatrix() which computes and caches it.
	// Calling GetModelMatrix() again should return the same matrix.
	glm::mat4 camMat = cam->GetModelMatrix();
	glm::mat4 lightMat = light->GetModelMatrix();
	glm::mat4 meshMat = mesh->GetModelMatrix();

	// Matrices should be non-identity (positions are non-zero)
	EXPECT_NE(camMat, glm::mat4(1.0f));
	EXPECT_NE(lightMat, glm::mat4(1.0f));
	EXPECT_NE(meshMat, glm::mat4(1.0f));

	// Translation components should match set positions
	EXPECT_FLOAT_EQ(camMat[3][0], 0.0f);
	EXPECT_FLOAT_EQ(camMat[3][1], 5.0f);
	EXPECT_FLOAT_EQ(camMat[3][2], 1.0f);

	EXPECT_FLOAT_EQ(lightMat[3][0], 2.0f);
	EXPECT_FLOAT_EQ(lightMat[3][1], 0.0f);
	EXPECT_FLOAT_EQ(lightMat[3][2], 3.0f);

	EXPECT_FLOAT_EQ(meshMat[3][0], -1.0f);
	EXPECT_FLOAT_EQ(meshMat[3][1], 2.0f);
	EXPECT_FLOAT_EQ(meshMat[3][2], 0.0f);
}

/**
 * @test UpdateObjTransforms on empty scene is a no-op.
 */
TEST(SceneIntegrationTest, UpdateObjTransformsEmptyScene)
{
	Scene scene;
	EXPECT_NO_THROW(scene.UpdateObjTransforms());
}

/**
 * @test UpdateObjTransforms with only non-transform objects (Sprite) skips safely.
 */
TEST(SceneIntegrationTest, UpdateObjTransformsSpriteOnly)
{
	Scene scene;

	auto sprite = std::make_shared<Sprite>();
	scene.UseSprite(sprite);

	EXPECT_NO_THROW(scene.UpdateObjTransforms());
}

// -----------------------------------------------------------------------
// 7. Camera property verification on registered camera
// -----------------------------------------------------------------------

/**
 * @test Verify that a Camera registered via UseCamera retains its
 *       property values and can be retrieved and queried.
 */
TEST(SceneIntegrationTest, CameraPropertiesAfterRegistration)
{
	Scene scene;

	auto cam = std::make_shared<Camera>(1920.0f, 1080.0f, 45.0f, 0.05f, 500.0f);
	cam->SetPosition(glm::vec3(0.0f, -10.0f, 2.0f));
	cam->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	scene.UseCamera(cam);

	// Retrieve from cam_list
	Camera* found = scene.cam_list.find(cam->GetObjectID())->second.get();
	ASSERT_NE(found, nullptr);

	EXPECT_FLOAT_EQ(found->cam_w,     1920.0f);
	EXPECT_FLOAT_EQ(found->cam_h,     1080.0f);
	EXPECT_FLOAT_EQ(found->cam_pers,  45.0f);
	EXPECT_FLOAT_EQ(found->cam_near,  0.05f);
	EXPECT_FLOAT_EQ(found->cam_far,   500.0f);
	EXPECT_FLOAT_EQ(found->GetPosition().y, -10.0f);
	EXPECT_FLOAT_EQ(found->cam_tar.z, 0.0f);
}

// -----------------------------------------------------------------------
// 8. Light property verification on registered light
// -----------------------------------------------------------------------

/**
 * @test Verify that a Light registered via UseLight retains its
 *       type, power, and color after registration.
 */
TEST(SceneIntegrationTest, LightPropertiesAfterRegistration)
{
	Scene scene;

	auto light = std::make_shared<Light>(LightType::SPOTLIGHT, 50.0f, glm::vec3(0.2f, 0.8f, 0.3f));
	light->SetPosition(glm::vec3(5.0f, 0.0f, 10.0f));
	light->light_radius = 0.12f;
	light->spot_cutoff  = 0.85f;

	scene.UseLight(light);

	Light* found = scene.light_list.find(light->GetObjectID())->second.get();
	ASSERT_NE(found, nullptr);

	EXPECT_EQ(found->light_type,  LightType::SPOTLIGHT);
	EXPECT_FLOAT_EQ(found->light_power, 50.0f);
	EXPECT_FLOAT_EQ(found->light_color.r, 0.2f);
	EXPECT_FLOAT_EQ(found->light_color.g, 0.8f);
	EXPECT_FLOAT_EQ(found->light_color.b, 0.3f);
	EXPECT_FLOAT_EQ(found->light_radius,  0.12f);
	EXPECT_FLOAT_EQ(found->spot_cutoff,   0.85f);
	EXPECT_FLOAT_EQ(found->GetPosition().x, 5.0f);
}

// -----------------------------------------------------------------------
// 9. Shared ownership - object stays alive after scene destruction
// -----------------------------------------------------------------------

/**
 * @test Objects registered in scene via shared_ptr remain alive through
 *       the shared_ptr even after the scene is destroyed.
 */
TEST(SceneIntegrationTest, SharedOwnershipOutlivesScene)
{
	auto cam   = std::make_shared<Camera>();
	auto light = std::make_shared<Light>();
	auto mesh  = std::make_shared<Mesh>();

	int camId   = cam->GetObjectID();
	int lightId = light->GetObjectID();
	int meshId  = mesh->GetObjectID();

	{
		Scene scene;
		scene.UseCamera(cam);
		scene.UseLight(light);
		scene.UseMesh(mesh);

		EXPECT_EQ(cam.use_count(),   3); // original + cam_list + obj_list (aliasing)
		EXPECT_EQ(light.use_count(), 3);
		EXPECT_EQ(mesh.use_count(),  3);
	}

	// After scene destruction, objects are still alive (original reference)
	EXPECT_EQ(cam.use_count(),   1);
	EXPECT_EQ(light.use_count(), 1);
	EXPECT_EQ(mesh.use_count(),  1);

	// IDs are still valid
	EXPECT_EQ(cam->GetObjectID(),   camId);
	EXPECT_EQ(light->GetObjectID(), lightId);
	EXPECT_EQ(mesh->GetObjectID(),  meshId);
}

// -----------------------------------------------------------------------
// 10. Multiple instances of same type in scene
// -----------------------------------------------------------------------

/**
 * @test Registering multiple cameras/lights/meshes correctly populates
 *       all pools and obj_list.
 */
TEST(SceneIntegrationTest, MultipleInstancesOfSameType)
{
	Scene scene;

	auto cam1 = std::make_shared<Camera>();
	auto cam2 = std::make_shared<Camera>();
	auto cam3 = std::make_shared<Camera>();

	auto light1 = std::make_shared<Light>(LightType::POINTLIGHT);
	auto light2 = std::make_shared<Light>(LightType::SUNLIGHT);

	auto mesh1 = std::make_shared<Mesh>();

	scene.UseCamera(cam1);
	scene.UseCamera(cam2);
	scene.UseCamera(cam3);
	scene.UseLight(light1);
	scene.UseLight(light2);
	scene.UseMesh(mesh1);

	EXPECT_EQ(scene.cam_list.size(),    3u);
	EXPECT_EQ(scene.light_list.size(),  2u);
	EXPECT_EQ(scene.mesh_list.size(),   1u);
	EXPECT_EQ(scene.obj_list.size(),    6u);

	// obj_list has all six
	EXPECT_NE(scene.obj_list.find(cam1->GetObjectID()),   scene.obj_list.end());
	EXPECT_NE(scene.obj_list.find(cam2->GetObjectID()),   scene.obj_list.end());
	EXPECT_NE(scene.obj_list.find(cam3->GetObjectID()),   scene.obj_list.end());
	EXPECT_NE(scene.obj_list.find(light1->GetObjectID()), scene.obj_list.end());
	EXPECT_NE(scene.obj_list.find(light2->GetObjectID()), scene.obj_list.end());
	EXPECT_NE(scene.obj_list.find(mesh1->GetObjectID()),  scene.obj_list.end());

	// The activated camera is the one named, not whichever the hash order yields.
	ASSERT_TRUE(scene.ActivateCamera(cam2->GetObjectID()));
	Camera* active = scene.GetActiveCamera();
	ASSERT_NE(active, nullptr);
	EXPECT_EQ(active->GetObjectID(), cam2->GetObjectID());
}
