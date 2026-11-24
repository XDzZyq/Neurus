/** @file test_scene_integration.cpp
 *  @brief Registration, lookup, transform updates and ownership across scene pools.
 */

#include <gtest/gtest.h>

#include <scene/Camera.hpp>
#include <scene/DebugLine.hpp>
#include <scene/DebugPoints.hpp>
#include <scene/Light.hpp>
#include <scene/Mesh.hpp>
#include <scene/Scene.hpp>
#include <scene/Sprite.hpp>

using namespace neurus;

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
	EXPECT_EQ(scene.obj_list.at(camId).shared(), cam);
	EXPECT_EQ(scene.cam_list.at(camId).shared(), cam);
	EXPECT_EQ(scene.obj_list.at(lightId).shared(), light);
	EXPECT_EQ(scene.light_list.at(lightId).shared(), light);
	EXPECT_EQ(scene.obj_list.at(meshId).shared(), mesh);
	EXPECT_EQ(scene.mesh_list.at(meshId).shared(), mesh);
	EXPECT_EQ(scene.obj_list.at(spriteId).shared(), sprite);
	EXPECT_EQ(scene.sprite_list.at(spriteId).shared(), sprite);
	EXPECT_EQ(scene.obj_list.at(dLineId).shared(), dLine);
	EXPECT_EQ(scene.dLine_list.at(dLineId).shared(), dLine);
	EXPECT_EQ(scene.obj_list.at(dPointsId).shared(), dPoints);
	EXPECT_EQ(scene.dPoints_list.at(dPointsId).shared(), dPoints);

	// --- Each type-specific pool has exactly one entry ---
	EXPECT_EQ(scene.cam_list.size(),      1u);
	EXPECT_EQ(scene.light_list.size(),    1u);
	EXPECT_EQ(scene.mesh_list.size(),     1u);
	EXPECT_EQ(scene.sprite_list.size(),   1u);
	EXPECT_EQ(scene.dLine_list.size(),    1u);
	EXPECT_EQ(scene.dPoints_list.size(),  1u);
	const Scene& view = scene;
	for (const auto& [uid, object] : scene.obj_list)
	{
		SCOPED_TRACE(uid);
		EXPECT_EQ(scene.GetObjectID(uid), object.get());
		EXPECT_EQ(view.GetObjectID(uid), object.get());
	}
	EXPECT_EQ(view.GetObjectID(-1), nullptr);
	EXPECT_EQ(scene.GetObjectID(99999), nullptr);
}

TEST(SceneIntegrationTest, UpdateObjTransformsIteratesAll)
{
	Scene scene;
	EXPECT_NO_THROW(scene.UpdateObjTransforms());
	scene.UseSprite(std::make_shared<Sprite>());
	EXPECT_NO_THROW(scene.UpdateObjTransforms());

	// Register objects with transforms
	auto cam     = std::make_shared<Camera>();
	auto light   = std::make_shared<Light>(LightType::SUNLIGHT);
	auto mesh    = std::make_shared<Mesh>();
	auto dLine   = std::make_shared<DebugLine>();
	auto dPoints = std::make_shared<DebugPoints>();

	// Edits through each polymorphic transform must update its owning object.
	ASSERT_EQ(dLine->GetTransform(), static_cast<Transform3D*>(dLine.get()));
	ASSERT_EQ(dPoints->GetTransform(), static_cast<Transform3D*>(dPoints.get()));

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
