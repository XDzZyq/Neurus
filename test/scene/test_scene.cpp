/**
 * @file test_scene.cpp
 * @brief Unit tests for Scene container class.
 *
 * TDD: RED (test written first) → GREEN (implementation verified).
 * All tests are pure CPU - no GPU required.
 *
 * Tests cover:
 * - Scene inherits UID with its own ID
 * - SceneModifStatus enum bitfield values
 * - Status tracking (UpdateSceneStatus, SetSceneStatus, CheckStatus, ResetStatus)
 * - ResPool<ObjectID> obj_list insert and lookup
 * - UseCamera registers in both cam_list and obj_list
 * - GetActiveCamera returns first camera
 * - UpdateObjTransforms stub no-crash
 */

#include <gtest/gtest.h>

#include <scene/Camera.h>
#include <scene/DebugLine.h>
#include <scene/DebugPoints.h>
#include <scene/Light.h>
#include <scene/Mesh.h>
#include <scene/Scene.h>
#include <scene/Sprite.h>
#include <scene/ObjectID.h>

using namespace neurus;

// -----------------------------------------------------------------------
// 1. Scene default construction
// -----------------------------------------------------------------------

/**
 * @test Scene inherits UID and has its own unique scene identifier.
 */
TEST(SceneTest, InheritsUID)
{
	Scene scene;

	// Scene should have a valid non-negative UID
	EXPECT_GE(scene.GetObjectID(), 0);

	// Two scenes should have different IDs
	Scene scene2;
	EXPECT_NE(scene.GetObjectID(), scene2.GetObjectID());
}

/**
 * @test Scene default-constructs with expected initial state.
 */
TEST(SceneTest, DefaultConstruction)
{
	Scene scene;

	// All pools should be empty on construction
	EXPECT_TRUE(scene.obj_list.empty());
	EXPECT_TRUE(scene.cam_list.empty());
	EXPECT_TRUE(scene.mesh_list.empty());
	EXPECT_TRUE(scene.light_list.empty());
	EXPECT_TRUE(scene.sprite_list.empty());
	EXPECT_TRUE(scene.dLine_list.empty());
	EXPECT_TRUE(scene.dPoints_list.empty());
}

// -----------------------------------------------------------------------
// 2. SceneModifStatus enum values
// -----------------------------------------------------------------------

/**
 * @test SceneModifStatus bitfield values are correct.
 */
TEST(SceneTest, SceneModifStatusValues)
{
	using S = Scene::SceneModifStatus;

	EXPECT_EQ(static_cast<int>(S::NoChanges), 0);
	EXPECT_EQ(static_cast<int>(S::ObjectTransChanged), 1 << 0);
	EXPECT_EQ(static_cast<int>(S::LightChanged), 1 << 1);
	EXPECT_EQ(static_cast<int>(S::CameraChanged), 1 << 2);
	EXPECT_EQ(static_cast<int>(S::ShaderChanged), 1 << 3);
	EXPECT_EQ(static_cast<int>(S::MaterialChanged), 1 << 4);
	EXPECT_EQ(static_cast<int>(S::SDFChanged), 1 << 8);

	// SceneChanged is the OR of the five core flags
	int expectedSceneChanged =
		static_cast<int>(S::ObjectTransChanged) |
		static_cast<int>(S::LightChanged) |
		static_cast<int>(S::CameraChanged) |
		static_cast<int>(S::ShaderChanged) |
		static_cast<int>(S::MaterialChanged);
	EXPECT_EQ(static_cast<int>(S::SceneChanged), expectedSceneChanged);
}

// -----------------------------------------------------------------------
// 3. Status tracking
// -----------------------------------------------------------------------

/**
 * @test CheckStatus returns true when flag is set, false when not.
 */
TEST(SceneTest, CheckStatusFlag)
{
	Scene scene;

	// Initially scene is at SceneChanged (all core flags set)
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::SceneChanged));

	// Reset to no changes
	scene.ResetStatus();
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));
}

/**
 * @test UpdateSceneStatus sets or clears individual flags.
 */
TEST(SceneTest, UpdateSceneStatus)
{
	Scene scene;

	// Reset to clean state
	scene.ResetStatus();
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::ObjectTransChanged));

	// Set ObjectTransChanged flag
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::ObjectTransChanged), true);
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::ObjectTransChanged));

	// Clear it
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::ObjectTransChanged), false);
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::ObjectTransChanged));
}

/**
 * @test UpdateSceneStatus sets multiple flags without clearing others.
 */
TEST(SceneTest, UpdateSceneStatusMultipleFlags)
{
	Scene scene;
	scene.ResetStatus();

	// Set LightChanged
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::LightChanged), true);
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));

	// Additionally set CameraChanged (should not clear LightChanged)
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::CameraChanged), true);
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));

	// Clear only LightChanged; CameraChanged should remain
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::LightChanged), false);
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));
}

/**
 * @test SetSceneStatus replaces entire status value.
 */
TEST(SceneTest, SetSceneStatusReplacesAll)
{
	Scene scene;
	scene.ResetStatus();

	// Set a specific flag
	scene.SetSceneStatus(static_cast<int>(Scene::SceneModifStatus::ShaderChanged), true);
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::ShaderChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));

	// Set a combo; should replace
	scene.SetSceneStatus(
		static_cast<int>(Scene::SceneModifStatus::CameraChanged) |
		static_cast<int>(Scene::SceneModifStatus::MaterialChanged),
		true);
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::MaterialChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::ShaderChanged));
}

/**
 * @test ResetStatus clears all flags to NoChanges.
 */
TEST(SceneTest, ResetStatus)
{
	Scene scene;

	// Scene starts with SceneChanged, so trigger some changes
	scene.ResetStatus();
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::LightChanged), true);
	scene.UpdateSceneStatus(static_cast<int>(Scene::SceneModifStatus::ShaderChanged), true);
	EXPECT_TRUE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));

	// Reset
	scene.ResetStatus();
	EXPECT_EQ(scene.CheckStatus(Scene::SceneModifStatus::NoChanges), false); // NoChanges == 0, so CheckStatus(0) is always false
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::ObjectTransChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::ShaderChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::MaterialChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::SDFChanged));
}

// -----------------------------------------------------------------------
// 4. obj_list pool and GetObjectID lookup
// -----------------------------------------------------------------------

/**
 * @test obj_list insert and GetObjectID lookup.
 */
TEST(SceneTest, ObjListInsertAndLookup)
{
	Scene scene;

	// Create a test ObjectID and insert directly into obj_list
	auto obj = std::make_shared<ObjectID>();
	int id = obj->GetObjectID();
	scene.obj_list[id] = obj;

	// Lookup via GetObjectID
	ObjectID* found = scene.GetObjectID(id);
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->GetObjectID(), id);

	// Lookup non-existent ID
	ObjectID* missing = scene.GetObjectID(-1);
	EXPECT_EQ(missing, nullptr);
}

/**
 * @test GetObjectID returns nullptr for empty scene.
 */
TEST(SceneTest, GetObjectIDEmptyScene)
{
	Scene scene;
	EXPECT_EQ(scene.GetObjectID(0), nullptr);
	EXPECT_EQ(scene.GetObjectID(42), nullptr);
}

// -----------------------------------------------------------------------
// 5. UseCamera registration in both pools
// -----------------------------------------------------------------------

/**
 * @test UseCamera registers Camera in both cam_list and obj_list.
 */
TEST(SceneTest, UseCameraRegistersInBothPools)
{
	Scene scene;
	auto cam = std::make_shared<Camera>();
	int id = cam->GetObjectID();

	EXPECT_TRUE(scene.cam_list.empty());
	EXPECT_TRUE(scene.obj_list.empty());

	scene.UseCamera(cam);

	// Camera should be in cam_list
	EXPECT_FALSE(scene.cam_list.empty());
	EXPECT_EQ(scene.cam_list.size(), 1u);
	auto camIt = scene.cam_list.find(id);
	ASSERT_NE(camIt, scene.cam_list.end());
	EXPECT_EQ(camIt->second, cam);

	// Camera should be in obj_list as well
	auto objIt = scene.obj_list.find(id);
	ASSERT_NE(objIt, scene.obj_list.end());
	EXPECT_EQ(objIt->second, cam);
}

/**
 * @test UseCamera alone activates nothing; ActivateCamera picks the view camera.
 */
TEST(SceneTest, UseCameraMultipleGetActiveCamera)
{
	Scene scene;
	auto cam1 = std::make_shared<Camera>();
	auto cam2 = std::make_shared<Camera>();

	// GetActiveCamera on empty scene
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);

	scene.UseCamera(cam1);
	scene.UseCamera(cam2);

	// obj_list should have both
	EXPECT_EQ(scene.obj_list.size(), 2u);

	// Registration does not activate: a scene with cameras may still have none
	// activated, in which case the Editor looks through its own camera.
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);
	EXPECT_EQ(scene.ActiveCameraID(), 0);

	// Activation is explicit and names exactly one camera — no dependence on
	// unordered_map bucket order.
	ASSERT_TRUE(scene.ActivateCamera(cam2->GetObjectID()));
	Camera* active = scene.GetActiveCamera();
	ASSERT_NE(active, nullptr);
	EXPECT_EQ(active->GetObjectID(), cam2->GetObjectID());
}

// -----------------------------------------------------------------------
// 6. UseLight, UseMesh, UseSprite registration
// -----------------------------------------------------------------------

/**
 * @test UseLight registers Light in both light_list and obj_list.
 */
TEST(SceneTest, UseLightRegistersInBothPools)
{
	Scene scene;
	auto light = std::make_shared<Light>();
	int id = light->GetObjectID();

	scene.UseLight(light);

	EXPECT_FALSE(scene.light_list.empty());
	EXPECT_EQ(scene.light_list.find(id)->second, light);
	EXPECT_EQ(scene.obj_list.find(id)->second, light);
}

/**
 * @test UseMesh registers Mesh in both mesh_list and obj_list.
 */
TEST(SceneTest, UseMeshRegistersInBothPools)
{
	Scene scene;
	auto mesh = std::make_shared<Mesh>();
	int id = mesh->GetObjectID();

	scene.UseMesh(mesh);

	EXPECT_FALSE(scene.mesh_list.empty());
	EXPECT_EQ(scene.mesh_list.find(id)->second, mesh);
	EXPECT_EQ(scene.obj_list.find(id)->second, mesh);
}

/**
 * @test UseSprite registers Sprite in both sprite_list and obj_list.
 */
TEST(SceneTest, UseSpriteRegistersInBothPools)
{
	Scene scene;
	auto sprite = std::make_shared<Sprite>();
	int id = sprite->GetObjectID();

	scene.UseSprite(sprite);

	EXPECT_FALSE(scene.sprite_list.empty());
	EXPECT_EQ(scene.sprite_list.find(id)->second, sprite);
	EXPECT_EQ(scene.obj_list.find(id)->second, sprite);
}

// -----------------------------------------------------------------------
// 7. UseDebugLine / UseDebugPoints registration
// -----------------------------------------------------------------------

/**
 * @test UseDebugLine registers DebugLine in dLine_list.
 */
TEST(SceneTest, UseDebugLineRegisters)
{
	Scene scene;
	auto dLine = std::make_shared<DebugLine>();
	int id = dLine->GetObjectID();

	scene.UseDebugLine(dLine);

	EXPECT_FALSE(scene.dLine_list.empty());
	EXPECT_EQ(scene.dLine_list.find(id)->second, dLine);
}

/**
 * @test UseDebugPoints registers DebugPoints in dPoints_list.
 */
TEST(SceneTest, UseDebugPointsRegisters)
{
	Scene scene;
	auto dPoints = std::make_shared<DebugPoints>();
	int id = dPoints->GetObjectID();

	scene.UseDebugPoints(dPoints);

	EXPECT_FALSE(scene.dPoints_list.empty());
	EXPECT_EQ(scene.dPoints_list.find(id)->second, dPoints);
}

// -----------------------------------------------------------------------
// 8. UpdateObjTransforms stub
// -----------------------------------------------------------------------

/**
 * @test UpdateObjTransforms stub does not crash.
 */
TEST(SceneTest, UpdateObjTransformsStub)
{
	Scene scene;

	// Should not crash or throw
	EXPECT_NO_THROW(scene.UpdateObjTransforms());

	// Call with objects registered
	auto cam = std::make_shared<Camera>();
	scene.UseCamera(cam);
	EXPECT_NO_THROW(scene.UpdateObjTransforms());
}

// ---------------------------------------------------------------------------
// 9. Const Scene accessors — GetActiveCamera and GetObjectID
// ---------------------------------------------------------------------------

/**
 * @test Calling GetActiveCamera() on a const Scene with a registered camera returns a non-null pointer.
 */
TEST(SceneConstAccessTest, ConstScene_GetActiveCamera_ReturnsValidPointer)
{
	Scene scene;
	auto cam = std::make_shared<Camera>();
	int camID = cam->GetObjectID();
	scene.UseCamera(cam);
	ASSERT_TRUE(scene.ActivateCamera(camID));

	// Obtain a const reference
	const Scene& constScene = scene;

	// const GetActiveCamera() must return non-null
	const Camera* active = constScene.GetActiveCamera();
	ASSERT_NE(active, nullptr);
	EXPECT_EQ(active->GetObjectID(), camID);

	// Verify it's the same camera
	EXPECT_EQ(active->o_type, ObjectID::GOType::GO_CAM);
}

/**
 * @test Calling GetActiveCamera() on a const empty Scene returns nullptr.
 */
TEST(SceneConstAccessTest, ConstScene_GetActiveCamera_EmptyScene_ReturnsNull)
{
	const Scene constScene;

	const Camera* active = constScene.GetActiveCamera();
	EXPECT_EQ(active, nullptr);
}

/**
 * @test Calling GetObjectID(int) on a const Scene returns a valid pointer for a registered object.
 */
TEST(SceneConstAccessTest, ConstScene_GetObjectID_ReturnsValidPointer)
{
	Scene scene;

	// Register a mesh
	auto mesh = std::make_shared<Mesh>();
	int meshID = mesh->GetObjectID();
	scene.UseMesh(mesh);

	// Register a camera
	auto cam = std::make_shared<Camera>();
	int camID = cam->GetObjectID();
	scene.UseCamera(cam);

	const Scene& constScene = scene;

	// Query mesh by ID
	const ObjectID* foundMesh = constScene.GetObjectID(meshID);
	ASSERT_NE(foundMesh, nullptr);
	EXPECT_EQ(foundMesh->GetObjectID(), meshID);
	EXPECT_EQ(foundMesh->o_type, ObjectID::GOType::GO_MESH);

	// Query camera by ID
	const ObjectID* foundCam = constScene.GetObjectID(camID);
	ASSERT_NE(foundCam, nullptr);
	EXPECT_EQ(foundCam->GetObjectID(), camID);
	EXPECT_EQ(foundCam->o_type, ObjectID::GOType::GO_CAM);
}

/**
 * @test Calling GetObjectID(int) on a const Scene for a non-existent ID returns nullptr.
 */
TEST(SceneConstAccessTest, ConstScene_GetObjectID_Nonexistent_ReturnsNull)
{
	const Scene constScene;

	const ObjectID* missing = constScene.GetObjectID(42);
	EXPECT_EQ(missing, nullptr);

	// Negative ID should also return nullptr
	const ObjectID* negative = constScene.GetObjectID(-1);
	EXPECT_EQ(negative, nullptr);
}
