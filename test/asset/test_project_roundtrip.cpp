/**
 * @file test_project_roundtrip.cpp
 * @brief Roundtrip serialization tests for Project Save -> Load.
 *
 * Verifies that Project::Save() followed by Project::Load() preserves
 * all scene data (cameras, meshes, lights) with exact parameter matching.
 *
 * The project now stores the ResourceManager pool FIRST (real objects,
 * polymorphically), then the Scene's ID references, which SceneComponent
 * resolves against the pool after load. These tests drive the same
 * component ordering as Application::BuildProject.
 *
 * Tests create bare Scene+RenderConfig+ResourceManager objects, register
 * them with a Project serializer via ResourceComponent/SceneComponent/
 * ConfigComponent, then Save/Load through the Project.
 *
 * TDD: RED (test written first) -> GREEN (implementation verified).
 * All tests are pure CPU -- no GPU required.
 */

#include <gtest/gtest.h>

#include <cstdio>   // std::remove

#include "asset/Project.h"
#include "asset/components/SceneComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/ConfigComponent.h"
#include "asset/data/MeshData.h"
#include "core/ResourceManager.h"
#include "render/RenderConfig.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

using namespace neurus;

// -----------------------------------------------------------------------
// RAII temporary file cleaner
// -----------------------------------------------------------------------

/**
 * @brief RAII wrapper that removes the temporary file on destruction.
 *
 * Ensures test-generated .neurus.json files are cleaned up even if
 * the test fails with an exception.
 */
struct TempFile
{
	std::string path;
	explicit TempFile(std::string p) : path(std::move(p)) {}
	~TempFile() { std::remove(path.c_str()); }
};

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

/**
 * @brief Creates a Project serializer with ResourceComponent + SceneComponent
 *        + ConfigComponent bound (mirrors Application::BuildProject ordering).
 *
 * @param scene     Scene object to serialize.
 * @param config    RenderConfig object to serialize.
 * @param resources ResourceManager pool (registered first).
 * @return Initialised project::Project ready for Save/Load.
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
// Roundtrip: Empty Scene
// -----------------------------------------------------------------------

/**
 * @test Save an empty project, load it back -- it stays empty. Nothing is
 *       injected to make it renderable: a camera-less scene is legal, because
 *       the viewport looks through the Editor's own camera unless the scene
 *       names an activated one.
 */
TEST(ProjectRoundtrip, EmptyScene)
{
	TempFile tmp("test_rt_empty.neurus.json");
	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;
		auto p = MakeProject(scene, config, resources);
		p.Save(tmp.path);
	}
	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	{
		auto p = MakeProject(loadedScene, loadedConfig, loadedResources);
		p.Load(tmp.path);
	}
	EXPECT_TRUE(loadedScene.cam_list.empty());
	EXPECT_EQ(loadedScene.GetActiveCamera(), nullptr);
	EXPECT_EQ(loadedScene.ActiveCameraID(), 0);
	EXPECT_TRUE(loadedScene.mesh_list.empty());
	EXPECT_TRUE(loadedScene.light_list.empty());
	EXPECT_TRUE(loadedScene.sprite_list.empty());
	EXPECT_TRUE(loadedScene.dLine_list.empty());
	EXPECT_TRUE(loadedScene.dPoints_list.empty());
}

// -----------------------------------------------------------------------
// Roundtrip: Camera Only
// -----------------------------------------------------------------------

/**
 * @test Create scene with a camera, save, load -- camera data matches.
 */
TEST(ProjectRoundtrip, CameraOnly)
{
	TempFile tmp("test_rt_camera.neurus.json");
	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;
		auto camera = resources.Load<Camera>();
		camera->cam_pers = 60.0f;
		camera->cam_near = 0.1f;
		camera->cam_far = 100.0f;
		camera->cam_tar = glm::vec3(0.0f, 0.0f, 1.0f);
		camera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
		scene.UseCamera(camera);
		scene.ActivateCamera(camera->GetObjectID());
		auto p = MakeProject(scene, config, resources);
		p.Save(tmp.path);
	}
	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	{
		auto p = MakeProject(loadedScene, loadedConfig, loadedResources);
		p.Load(tmp.path);
	}
	ASSERT_EQ(loadedScene.cam_list.size(), 1u);
	auto* loadedCam = loadedScene.cam_list.begin()->second.get();
	ASSERT_NE(loadedCam, nullptr);
	EXPECT_FLOAT_EQ(loadedCam->cam_pers, 60.0f);
	EXPECT_FLOAT_EQ(loadedCam->cam_near, 0.1f);
	EXPECT_FLOAT_EQ(loadedCam->cam_far, 100.0f);
	EXPECT_EQ(loadedCam->cam_tar, glm::vec3(0.0f, 0.0f, 1.0f));
	EXPECT_EQ(loadedCam->GetPosition(), glm::vec3(0.0f, -5.0f, 2.0f));
	EXPECT_EQ(loadedCam->o_type, ObjectID::GOType::GO_CAM);
}

// -----------------------------------------------------------------------
// Roundtrip: Mesh with pooled MeshData
// -----------------------------------------------------------------------

/**
 * @test Create scene with a mesh referencing pooled MeshData, save, load --
 *       o_meshDataId, flags and name match, and o_mesh re-wires to the pool.
 */
TEST(ProjectRoundtrip, MeshWithOBJ)
{
	TempFile tmp("test_rt_mesh.neurus.json");
	int meshDataId = 0;
	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;
		auto meshData = resources.Load<MeshData>("res/obj/sphere.obj");
		meshDataId = meshData->GetObjectID();
		auto mesh = resources.Load<Mesh>(meshData);
		mesh->using_shadow = false;
		mesh->using_sdf = false;
		mesh->o_name = "TestSphere";
		scene.UseMesh(mesh);
		auto p = MakeProject(scene, config, resources);
		p.Save(tmp.path);
	}
	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	{
		auto p = MakeProject(loadedScene, loadedConfig, loadedResources);
		p.Load(tmp.path);
	}
	ASSERT_EQ(loadedScene.mesh_list.size(), 1u);
	auto* loadedMesh = loadedScene.mesh_list.begin()->second.get();
	ASSERT_NE(loadedMesh, nullptr);
	EXPECT_EQ(loadedMesh->o_meshDataId, meshDataId);
	EXPECT_FALSE(loadedMesh->using_shadow);
	EXPECT_FALSE(loadedMesh->using_sdf);
	EXPECT_EQ(loadedMesh->o_name, "TestSphere");
	EXPECT_EQ(loadedMesh->o_type, ObjectID::GOType::GO_MESH);
	// The pooled MeshData is re-wired and its content reloaded from disk.
	ASSERT_NE(loadedMesh->o_mesh, nullptr);
	EXPECT_EQ(loadedMesh->o_mesh->GetObjectID(), meshDataId);
	EXPECT_TRUE(loadedMesh->o_mesh->GetVertexCount() > 0);
}

// -----------------------------------------------------------------------
// Roundtrip: Point Light
// -----------------------------------------------------------------------

/**
 * @test Create scene with a point light, save, load -- light params match.
 */
TEST(ProjectRoundtrip, LightPoint)
{
	TempFile tmp("test_rt_light.neurus.json");
	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;
		auto light = resources.Load<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f, 0.8f, 0.6f));
		light->SetPosition(glm::vec3(3.0f, 3.0f, 3.0f));
		light->SetRadius(0.01f);
		scene.UseLight(light);
		auto p = MakeProject(scene, config, resources);
		p.Save(tmp.path);
	}
	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	{
		auto p = MakeProject(loadedScene, loadedConfig, loadedResources);
		p.Load(tmp.path);
	}
	ASSERT_EQ(loadedScene.light_list.size(), 1u);
	auto* loadedLight = loadedScene.light_list.begin()->second.get();
	ASSERT_NE(loadedLight, nullptr);
	EXPECT_EQ(loadedLight->light_type, POINTLIGHT);
	EXPECT_FLOAT_EQ(loadedLight->light_power, 10.0f);
	EXPECT_EQ(loadedLight->light_color, glm::vec3(1.0f, 0.8f, 0.6f));
	EXPECT_FLOAT_EQ(loadedLight->light_radius, 0.01f);
	EXPECT_EQ(loadedLight->GetPosition(), glm::vec3(3.0f, 3.0f, 3.0f));
	EXPECT_EQ(loadedLight->o_type, ObjectID::GOType::GO_LIGHT);
}

// -----------------------------------------------------------------------
// Roundtrip: Full Scene (camera + mesh + light)
// -----------------------------------------------------------------------

/**
 * @test Create a full scene with camera, mesh, and light. Save, load --
 *       all three objects present with correct parameters.
 */
TEST(ProjectRoundtrip, FullScene)
{
	TempFile tmp("test_rt_full.neurus.json");
	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;

		// Camera
		auto camera = resources.Load<Camera>();
		camera->SetPosition(glm::vec3(0.0f, 10.0f, 3.0f));
		camera->cam_tar = glm::vec3(0.0f, 0.0f, 1.0f);
		camera->cam_pers = 45.0f;
		scene.UseCamera(camera);
		scene.ActivateCamera(camera->GetObjectID());

		// Mesh
		auto meshData = resources.Load<MeshData>("obj/cube.obj");
		auto mesh = resources.Load<Mesh>(meshData);
		mesh->SetPosition(glm::vec3(1.0f, 0.0f, 0.0f));
		scene.UseMesh(mesh);

		// Light
		auto light = resources.Load<Light>(POINTLIGHT, 20.0f, glm::vec3(0.2f, 0.5f, 1.0f));
		light->SetPosition(glm::vec3(-2.0f, 0.0f, 5.0f));
		light->SetRadius(0.1f);
		scene.UseLight(light);

		auto p = MakeProject(scene, config, resources);
		p.Save(tmp.path);
	}
	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	{
		auto p = MakeProject(loadedScene, loadedConfig, loadedResources);
		p.Load(tmp.path);
	}

	// Pool sizes
	EXPECT_EQ(loadedScene.cam_list.size(), 1u);
	EXPECT_EQ(loadedScene.mesh_list.size(), 1u);
	EXPECT_EQ(loadedScene.light_list.size(), 1u);

	// --- Camera validation ---
	auto* loadedCam = loadedScene.cam_list.begin()->second.get();
	ASSERT_NE(loadedCam, nullptr);
	EXPECT_EQ(loadedCam->GetPosition(), glm::vec3(0.0f, 10.0f, 3.0f));
	EXPECT_EQ(loadedCam->cam_tar, glm::vec3(0.0f, 0.0f, 1.0f));
	EXPECT_FLOAT_EQ(loadedCam->cam_pers, 45.0f);
	EXPECT_EQ(loadedCam->o_type, ObjectID::GOType::GO_CAM);

	// --- Mesh validation ---
	auto* loadedMesh = loadedScene.mesh_list.begin()->second.get();
	ASSERT_NE(loadedMesh, nullptr);
	EXPECT_NE(loadedMesh->o_meshDataId, 0);
	EXPECT_EQ(loadedMesh->GetPosition(), glm::vec3(1.0f, 0.0f, 0.0f));
	EXPECT_EQ(loadedMesh->o_type, ObjectID::GOType::GO_MESH);

	// --- Light validation ---
	auto* loadedLight = loadedScene.light_list.begin()->second.get();
	ASSERT_NE(loadedLight, nullptr);
	EXPECT_EQ(loadedLight->light_type, POINTLIGHT);
	EXPECT_FLOAT_EQ(loadedLight->light_power, 20.0f);
	EXPECT_EQ(loadedLight->light_color, glm::vec3(0.2f, 0.5f, 1.0f));
	EXPECT_FLOAT_EQ(loadedLight->light_radius, 0.1f);
	EXPECT_EQ(loadedLight->GetPosition(), glm::vec3(-2.0f, 0.0f, 5.0f));
	EXPECT_EQ(loadedLight->o_type, ObjectID::GOType::GO_LIGHT);
}

// -----------------------------------------------------------------------
// Roundtrip: Selection state
// -----------------------------------------------------------------------

/**
 * @test Selection (stored as pointers, persisted as UIDs) survives a
 *       save/load roundtrip: selected set and active object are restored
 *       against the reloaded objects by UID.
 */
TEST(ProjectRoundtrip, SelectionState)
{
	TempFile tmp("test_rt_selection.neurus.json");
	int meshUid = 0;
	int lightUid = 0;
	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;

		auto meshData = resources.Load<MeshData>("obj/cube.obj");
		auto mesh = resources.Load<Mesh>(meshData);
		scene.UseMesh(mesh);
		meshUid = mesh->GetObjectID();

		auto light = resources.Load<Light>(POINTLIGHT, 5.0f, glm::vec3(1.0f));
		scene.UseLight(light);
		lightUid = light->GetObjectID();

		// Select mesh (replace), then add light — light becomes active.
		scene.selections.Select(mesh.get(), false);
		scene.selections.Select(light.get(), true);

		auto p = MakeProject(scene, config, resources);
		p.Save(tmp.path);
	}
	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	{
		auto p = MakeProject(loadedScene, loadedConfig, loadedResources);
		p.Load(tmp.path);
	}

	// Both objects selected, light is active.
	EXPECT_EQ(loadedScene.selections.GetSelectionCount(), 2u);

	const ObjectID* loadedMesh = loadedScene.GetObjectID(meshUid);
	const ObjectID* loadedLight = loadedScene.GetObjectID(lightUid);
	ASSERT_NE(loadedMesh, nullptr);
	ASSERT_NE(loadedLight, nullptr);
	EXPECT_TRUE(loadedScene.selections.IsSelected(loadedMesh));
	EXPECT_TRUE(loadedScene.selections.IsSelected(loadedLight));

	const ObjectID* active = loadedScene.selections.GetActiveObject();
	ASSERT_NE(active, nullptr);
	EXPECT_EQ(active->GetObjectID(), lightUid);
}

/**
 * @test A scene with no selection roundtrips to an empty selection
 *       (the selection block is present but empty).
 */
TEST(ProjectRoundtrip, EmptySelection)
{
	TempFile tmp("test_rt_empty_selection.neurus.json");
	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;
		auto meshData = resources.Load<MeshData>("obj/cube.obj");
		auto mesh = resources.Load<Mesh>(meshData);
		scene.UseMesh(mesh);
		auto p = MakeProject(scene, config, resources);
		p.Save(tmp.path);
	}
	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	{
		auto p = MakeProject(loadedScene, loadedConfig, loadedResources);
		p.Load(tmp.path);
	}
	EXPECT_EQ(loadedScene.selections.GetSelectionCount(), 0u);
	EXPECT_EQ(loadedScene.selections.GetActiveObject(), nullptr);
}
