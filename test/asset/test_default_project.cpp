/**
 * @file test_default_project.cpp
 * @brief Tests loading and validating the default project from res/default.neurus.json.
 *
 * Verifies that the shipped default project file can be loaded without errors
 * and contains the expected scene objects (camera, mesh, light, environment).
 *
 * Tests use the new component-based Project API: create bare Scene+RenderConfig
 * objects, register SceneComponent and ConfigComponent, then call Project::Load().
 *
 * TDD: RED (test written first) -> GREEN (implementation verified).
 * All tests are pure CPU -- no GPU required.
 */

#include <gtest/gtest.h>

#include <string>

#include "asset/Project.h"
#include "asset/components/SceneComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/ConfigComponent.h"
#include "asset/data/ImageData.h"
#include "asset/data/MeshData.h"
#include "core/ResourceManager.h"
#include "core/RenderConfig.h"
#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

using namespace neurus;

// -----------------------------------------------------------------------
// Path helper
// -----------------------------------------------------------------------

/**
 * @brief Constructs the full path to res/default.neurus.json.
 *
 * Uses the compile-time TEST_SOURCE_DIR definition (set in CMakeLists.txt)
 * to locate the resource file regardless of the current working directory.
 *
 * @return Absolute path to the default project file.
 */
static std::string DefaultProjectPath()
{
	return std::string(TEST_SOURCE_DIR) + "/res/default.neurus.json";
}

// -----------------------------------------------------------------------
// Helper
// -----------------------------------------------------------------------

/**
 * @brief Creates a Project serializer with ResourceComponent + SceneComponent +
 *        ConfigComponent bound (mirrors Application::BuildProject ordering).
 *
 * @param scene     Scene object to deserialize into.
 * @param config    RenderConfig object to deserialize into.
 * @param resources ResourceManager pool (registered first; Scene resolves its
 *                  ID references against it).
 * @return Initialised project::Project ready for Load().
 */
static project::Project MakeProject(Scene& scene, RenderConfig& config, ResourceManager& resources)
{
	project::Project p;
	p.Register<project::ResourceComponent>(resources);
	p.Register<project::SceneComponent>(scene, resources);
	p.Register<project::ConfigComponent>(config);
	return p;
}

// -----------------------------------------------------------------------
// DefaultProject: Loads without exception
// -----------------------------------------------------------------------

/**
 * @test Opening the default project file does not throw.
 */
TEST(DefaultProject, LoadsWithoutException)
{
	Scene scene;
	RenderConfig config;
	ResourceManager resources;
	EXPECT_NO_THROW({
		auto p = MakeProject(scene, config, resources);
		p.Load(DefaultProjectPath());
	});
}

// -----------------------------------------------------------------------
// DefaultProject: Has camera
// -----------------------------------------------------------------------

/**
 * @test The default project contains at least one camera with valid FOV
 *       and near/far clip planes.
 */
TEST(DefaultProject, HasCamera)
{
	Scene scene;
	RenderConfig config;
	ResourceManager resources;
	{
		auto p = MakeProject(scene, config, resources);
		p.Load(DefaultProjectPath());
	}
	EXPECT_GE(scene.cam_list.size(), 1u);
	ASSERT_GE(scene.cam_list.size(), 1u);
	auto* cam = scene.cam_list.begin()->second.get();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->o_type, ObjectID::GOType::GO_CAM);
	EXPECT_GT(cam->cam_pers, 0.0f);
	EXPECT_GT(cam->cam_near, 0.0f);
	EXPECT_GT(cam->cam_far, cam->cam_near);
}

// -----------------------------------------------------------------------
// DefaultProject: Has mesh with valid OBJ path
// -----------------------------------------------------------------------

/**
 * @test The default project contains at least one mesh with a non-empty
 *       OBJ path configured for rendering.
 */
TEST(DefaultProject, HasMesh)
{
	Scene scene;
	RenderConfig config;
	ResourceManager resources;
	{
		auto p = MakeProject(scene, config, resources);
		p.Load(DefaultProjectPath());
	}
	EXPECT_GE(scene.mesh_list.size(), 1u);
	ASSERT_GE(scene.mesh_list.size(), 1u);
	auto* mesh = scene.mesh_list.begin()->second.get();
	ASSERT_NE(mesh, nullptr);
	EXPECT_EQ(mesh->o_type, ObjectID::GOType::GO_MESH);
	// Geometry is a pooled MeshData reference in the new format.
	EXPECT_NE(mesh->o_meshDataId, 0);
	ASSERT_NE(mesh->o_mesh, nullptr);
	EXPECT_EQ(mesh->o_mesh->GetObjectID(), mesh->o_meshDataId);
}

// -----------------------------------------------------------------------
// DefaultProject: Has light
// -----------------------------------------------------------------------

/**
 * @test The default project contains at least one light with positive power
 *       and a valid light type.
 */
TEST(DefaultProject, HasLight)
{
	Scene scene;
	RenderConfig config;
	ResourceManager resources;
	{
		auto p = MakeProject(scene, config, resources);
		p.Load(DefaultProjectPath());
	}
	EXPECT_GE(scene.light_list.size(), 1u);
	ASSERT_GE(scene.light_list.size(), 1u);
	auto* light = scene.light_list.begin()->second.get();
	ASSERT_NE(light, nullptr);
	EXPECT_EQ(light->o_type, ObjectID::GOType::GO_LIGHT);
	EXPECT_GT(light->light_power, 0.0f);
}

// -----------------------------------------------------------------------
// DefaultProject: Has environment in env_list
// -----------------------------------------------------------------------

/**
 * @test The default project file contains an Environment in env_list
 *       with the expected IBL equirect path.
 */
TEST(DefaultProject, HasEnvironment)
{
	Scene scene;
	RenderConfig config;
	ResourceManager resources;
	{
		auto p = MakeProject(scene, config, resources);
		p.Load(DefaultProjectPath());
	}
	EXPECT_FALSE(scene.env_list.empty());
	auto env = scene.env_list.begin()->second;
	ASSERT_NE(env, nullptr);
	// The Environment wraps the pooled ImageData; the path lives in the
	// data layer (ImageData::GetPath), not on the scene object.
	ASSERT_NE(env->GetEquirectData(), nullptr);
	EXPECT_EQ(env->GetEquirectData()->GetPath(), "res/tex/hdr/room.hdr");
	EXPECT_TRUE(env->GetEquirectData()->IsValid());
}
