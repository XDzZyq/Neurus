/**
 * @file test_scene_serialize.cpp
 * @brief Scene serialization tests: reference-based persistence + pool resolution.
 *
 * Covers:
 * - Full round-trip through project::Project with ResourceComponent FIRST
 *   (pool) then SceneComponent (Scene ID references resolved against the pool):
 *   typed pools, obj_list, selection restore by ID, and data-resource wiring
 *   (Mesh -> MeshData + Shader, Environment -> ImageData).
 * - Legacy project files (no "m_resources" node, old full-pool "m_scene")
 *   degrade to an empty scene + default-camera fallback without throwing.
 *
 * Pure CPU -- no GPU required.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "asset/Project.h"
#include "asset/components/SceneComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/ConfigComponent.h"
#include "core/ResourceManager.h"
#include "render/RenderConfig.h"
#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "asset/data/ImageData.h"
#include "asset/data/MeshData.h"
#include "render/shaders/RenderShader.h"

// Force-link the polymorphic registration TUs (static libs).
#include "scene/registrations/TypeRegistration.h"
#include "asset/registrations/DataRegistration.h"
#include "render/registrations/ShaderRegistration.h"

using namespace neurus;

namespace
{

struct TempFile
{
	std::string path;
	explicit TempFile(std::string p) : path(std::move(p)) {}
	~TempFile() { std::remove(path.c_str()); }
};

} // anonymous namespace

// -----------------------------------------------------------------------
// Full round-trip: pool first, then Scene references
// -----------------------------------------------------------------------

/**
 * @test A scene with camera, mesh (MeshData + Shader), light, and environment
 *       (ImageData) round-trips: typed pools re-populated from the pool, the
 *       data-resource pointers re-wired, obj_list rebuilt, selection restored.
 */
TEST(SceneSerialize, FullRoundtrip)
{
	TempFile tmp("test_scene_serialize_rt.neurus.json");

	int meshUid = 0;
	int meshDataUid = 0;
	int envUid = 0;
	int shaderUid = 0;
	int imageDataUid = 0;

	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;

		auto camera = resources.Load<Camera>();
		camera->cam_tar = glm::vec3(0.0f, 1.0f, 0.0f);
		scene.UseCamera(camera);

		auto meshData = resources.Load<MeshData>("res/obj/sphere.obj");
		meshDataUid = meshData->GetObjectID();
		auto mesh = resources.Load<Mesh>(meshData);
		auto shader = resources.Load<RenderShader>("TestShader", "", "");
		mesh->SetObjShader(shader);
		scene.UseMesh(mesh);
		// A non-identity TRS, so the matrix assertions below cannot pass by
		// accident on an identity matrix.
		mesh->SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
		mesh->SetRotation(glm::vec3(0.0f, 0.0f, 90.0f)); // yaw
		mesh->SetScale(glm::vec3(2.0f));
		meshUid = mesh->GetObjectID();
		shaderUid = shader->GetObjectID();

		auto light = resources.Load<Light>(SUNLIGHT, 3.0f, glm::vec3(1.0f));
		scene.UseLight(light);

		auto imageData = resources.Load<ImageData>("res/tex/hdr/room.hdr");
		auto env = resources.Load<Environment>(imageData);
		scene.UseEnvironment(env);
		envUid = env->GetObjectID();
		imageDataUid = imageData->GetObjectID();

		scene.selections.Select(mesh.get(), false);

		project::Project p;
		p.Register<project::ResourceComponent>(resources);
		p.Register<project::SceneComponent>(scene, resources);
		p.Register<project::ConfigComponent>(config);
		p.Save(tmp.path);
	}

	// --- Load into fresh objects ---
	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	{
		project::Project p;
		p.Register<project::ResourceComponent>(loadedResources);
		p.Register<project::SceneComponent>(loadedScene, loadedResources);
		p.Register<project::ConfigComponent>(loadedConfig);
		p.Load(tmp.path);
	}

	// Typed pools re-populated from the pool.
	EXPECT_EQ(loadedScene.cam_list.size(), 1u);
	ASSERT_EQ(loadedScene.mesh_list.size(), 1u);
	EXPECT_EQ(loadedScene.light_list.size(), 1u);
	ASSERT_EQ(loadedScene.env_list.size(), 1u);

	// obj_list aliases the typed pools (master pool rebuilt).
	ASSERT_NE(loadedScene.GetObjectID(meshUid), nullptr);
	ASSERT_NE(loadedScene.GetObjectID(envUid), nullptr);

	// Mesh data-resource wiring: MeshData + Shader by pooled ID.
	auto* loadedMesh = loadedScene.mesh_list.begin()->second.get();
	ASSERT_NE(loadedMesh, nullptr);
	EXPECT_EQ(loadedMesh->o_meshDataId, meshDataUid);
	EXPECT_NE(loadedMesh->o_meshDataId, 0);
	ASSERT_NE(loadedMesh->o_mesh, nullptr);
	EXPECT_EQ(loadedMesh->o_mesh->GetObjectID(), loadedMesh->o_meshDataId);
	EXPECT_GT(loadedMesh->o_mesh->GetVertexCount(), 0u); // content reloaded from res/

	// Transform: the raw TRS round-trips as data, and the cached model matrix -
	// the value the geometry/shadow passes actually read - is rebuilt for it.
	// Without that rebuild the object draws at the origin with unit scale while
	// the Property panel shows the values below.
	EXPECT_EQ(loadedMesh->GetPosition(), glm::vec3(1.0f, 2.0f, 3.0f));
	Transform3D expected;
	expected.SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	expected.SetRotation(glm::vec3(0.0f, 0.0f, 90.0f));
	expected.SetScale(glm::vec3(2.0f));
	EXPECT_EQ(loadedMesh->GetModelMatrix(), expected.GetModelMatrix());
	EXPECT_NE(loadedMesh->GetModelMatrix(), glm::mat4(1.0f));

	// Camera: the cached view matrix is a computed value as well, and the load
	// path's only other rebuild of it is an aspect-ratio setter taking a detour
	// through ApplyViewportToActiveCamera().
	Camera* loadedCam = loadedScene.GetActiveCamera();
	ASSERT_NE(loadedCam, nullptr);
	EXPECT_EQ(loadedCam->GetViewMatrix(),
	          glm::lookAt(loadedCam->GetPosition(), loadedCam->cam_tar,
	                      glm::vec3(0.0f, 0.0f, 1.0f)))
	    << "the loaded camera's view matrix must match its loaded position/target";

	EXPECT_EQ(loadedMesh->o_shaderId, shaderUid);
	ASSERT_NE(loadedMesh->o_shader, nullptr);
	EXPECT_EQ(loadedMesh->o_shader->GetObjectID(), shaderUid);
	EXPECT_EQ(loadedMesh->o_shader->GetName(), "TestShader");

	// Environment data-resource wiring: ImageData by pooled ID.
	auto* loadedEnv = loadedScene.env_list.begin()->second.get();
	ASSERT_NE(loadedEnv, nullptr);
	EXPECT_EQ(loadedEnv->o_imageDataId, imageDataUid);
	ASSERT_NE(loadedEnv->GetEquirectData(), nullptr);
	EXPECT_EQ(loadedEnv->GetEquirectData()->GetObjectID(), imageDataUid);
	EXPECT_TRUE(loadedEnv->GetEquirectData()->IsValid()); // reloaded from res/

	// Selection restored by UID against the rebuilt obj_list.
	EXPECT_EQ(loadedScene.selections.GetSelectionCount(), 1u);
	const ObjectID* selected = loadedScene.selections.GetActiveObject();
	ASSERT_NE(selected, nullptr);
	EXPECT_EQ(selected->GetObjectID(), meshUid);
}

// -----------------------------------------------------------------------
// Legacy project file degrades gracefully
// -----------------------------------------------------------------------

/**
 * @test An old-format project (no "m_resources" node, full-pool "m_scene"
 *       node) loads without throwing: the pool stays empty, the Scene fails
 *       to read its ID lists, and the default-camera fallback applies.
 */
TEST(SceneSerialize, LegacyFileDegrades)
{
	TempFile tmp("test_scene_serialize_legacy.neurus.json");

	// Simulate an old-format file: no m_resources, m_scene with old keys.
	// Wrapped in "project" (Project::Load reads via make_nvp("project", *this)).
	{
		std::ofstream out(tmp.path);
		out << R"({
			"project": {
				"m_scene": { "cam_list": [], "mesh_list": [], "light_list": [],
				             "sprite_list": [], "dLine_list": [], "dPoints_list": [],
				             "env_list": [] }
			}
		})";
	}

	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	EXPECT_NO_THROW({
		project::Project p;
		p.Register<project::ResourceComponent>(loadedResources);
		p.Register<project::SceneComponent>(loadedScene, loadedResources);
		p.Register<project::ConfigComponent>(loadedConfig);
		p.Load(tmp.path);
	});

	// No pooled objects survived; the fallback adds a default pooled camera.
	EXPECT_EQ(loadedResources.Size(), 1u); // the default camera
	EXPECT_EQ(loadedScene.cam_list.size(), 1u);
	EXPECT_TRUE(loadedScene.mesh_list.empty());
	EXPECT_TRUE(loadedScene.light_list.empty());
	EXPECT_EQ(loadedScene.selections.GetSelectionCount(), 0u);
}

/**
 * @test A pooled-but-orphaned mesh (deleted from the scene but still in the
 *       pool — referenced by undo history) gets its data references wired
 *       after load. The GPU caches mirror the pool, so the orphan must be
 *       uploadable for undo-of-delete after a project reload.
 */
TEST(SceneSerialize, PooledOrphanMesh_DataRefWiredAfterLoad)
{
	TempFile tmp("test_scene_serialize_orphan.neurus.json");

	int orphanUid = 0;
	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;

		auto meshData = resources.Load<MeshData>("res/obj/sphere.obj");
		auto orphan = resources.Load<Mesh>(meshData);
		orphanUid = orphan->GetObjectID();
		// NOTE: the orphan is deliberately NOT registered in the scene —
		// simulating a deleted object whose pooled resource survives.

		project::Project p;
		p.Register<project::ResourceComponent>(resources);
		p.Register<project::SceneComponent>(scene, resources);
		p.Register<project::ConfigComponent>(config);
		p.Save(tmp.path);
	}

	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	{
		project::Project p;
		p.Register<project::ResourceComponent>(loadedResources);
		p.Register<project::SceneComponent>(loadedScene, loadedResources);
		p.Register<project::ConfigComponent>(loadedConfig);
		p.Load(tmp.path);
	}

	ASSERT_TRUE(loadedScene.mesh_list.empty()); // orphan is NOT in the scene
	auto orphan = loadedResources.Get<Mesh>(orphanUid);
	ASSERT_NE(orphan, nullptr);
	EXPECT_NE(orphan->o_meshDataId, 0);        // persisted by the pool
	ASSERT_NE(orphan->o_mesh, nullptr);        // wired by the pool scan
	EXPECT_EQ(orphan->o_mesh->GetObjectID(), orphan->o_meshDataId);
}
