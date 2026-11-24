/** @file test_scene.cpp
 *  @brief Scene status flags and const access to scene objects.
 */

#include <gtest/gtest.h>

#include <scene/Camera.hpp>
#include <scene/Mesh.hpp>
#include <scene/Scene.hpp>
#include <scene/ObjectID.hpp>

using namespace neurus;

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
	scene.ResetStatus();
	EXPECT_EQ(scene.CheckStatus(Scene::SceneModifStatus::NoChanges), false); // NoChanges == 0, so CheckStatus(0) is always false
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::ObjectTransChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::LightChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::CameraChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::ShaderChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::MaterialChanged));
	EXPECT_FALSE(scene.CheckStatus(Scene::SceneModifStatus::SDFChanged));
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
