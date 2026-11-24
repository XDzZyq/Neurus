/**
 * @file TestDeferredScene.hpp
 * @brief Header-only helper for loading the deferred shading test scene in GPU tests.
 *
 * Provides BuildDeferredScene() which loads a sphere OBJ mesh, uploads it to GPU
 * via Mesh::UploadToGPU(), creates a default camera, point light, and PBR material,
 * and returns all resources in a DeferredSceneResources struct.
 *
 * Usage:
 * @code
 *   auto path = ResolveAssetPath("res/obj/sphere.obj");
 *   auto scene = BuildDeferredScene(resources, path);
 *   // Meshes registered in scene.mesh_list — GeometryPass and ShadowDepthPass
 *   // iterate mesh_list directly via RenderCache::GetMeshGPU().
 *   m_geometryPass->Record(cmd, cache, ctx);
 * @endcode
 *
 * Scene layout:
 *   - Sphere at origin, scaled by 0.25
 *   - Camera at (0, -5, 2) looking at origin, 60° FOV (Z-up: +Y=forward, +Z=up)
 *   - Point light at (2, 2, 2), power 10, radius 10, white
 *   - Material: metallic 0, roughness 0.5, albedo white
 */

#pragma once

#include "asset/ResourceManager.hpp"
#include "core/Log.hpp"

#include "asset/data/MeshData.hpp"
#include "scene/Camera.hpp"
#include "scene/Light.hpp"
#include "scene/Material.hpp"
#include "scene/Mesh.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <memory>
#include <string>

namespace neurus {
namespace test {

/**
 * @brief Aggregated result of building the deferred shading test scene.
 *
 * Owns all scene resources via shared_ptr. The mesh is registered in
 * `scene.mesh_list` for direct iteration by GeometryPass.
 */
struct DeferredSceneResources
{
	std::shared_ptr<Mesh>     mesh;
	std::shared_ptr<Camera>   camera;
	std::shared_ptr<Light>    light;
	std::shared_ptr<Material> material;
	std::unique_ptr<Scene>    scene;
};

/**
 * @brief Builds a deferred shading test scene: sphere mesh + camera + light + material.
 *
 * Loads the OBJ file at @p meshPath, creates a PBR material (metallic=0,
 * roughness=0.5, albedo=white), registers the mesh in a Scene with
 * 0.25x scale transform, and sets up a default camera and point light.
 *
 * Camera: position (0, -5, 2), target (0, 0, 0), 60° FOV (Z-up: +Y=forward, +Z=up).
 * Light:  POINTLIGHT at (2, 2, 2), power 10, radius 10, colour white.
 *
 * @param meshPath         Path to the sphere OBJ file (caller resolves).
 * @param width            Viewport width for camera aspect ratio (default 256).
 * @param height           Viewport height for camera aspect ratio (default 256).
 * @return Fully populated DeferredSceneResources.
 */
inline DeferredSceneResources BuildDeferredScene(
	ResourceManager& resources,
	const std::string& meshPath,
	float width = 256.0f,
	float height = 256.0f)
{
	DeferredSceneResources res;

	// --- Load sphere OBJ ---
	auto meshData = resources.Load<MeshData>();
	if (!meshData->LoadObj(meshPath))
	{
		NEURUS_ERR("BuildDeferredScene: Failed to load " << meshPath);
		return res;
	}

	const auto& rawMesh = meshData->GetMeshData();
	if (rawMesh.dataArray.empty() || rawMesh.indexArray.empty())
	{
		NEURUS_ERR("BuildDeferredScene: Empty geometry in " << meshPath);
		return res;
	}

	// --- Create material (metallic=0, roughness=0.5, albedo=white) ---
	res.material = std::make_shared<Material>();
	res.material->SetMatParam(Material::MAT_METAL, 0.0f);
	res.material->SetMatParam(Material::MAT_ROUGH, 0.5f);
	res.material->SetMatParam(Material::MAT_ALBEDO, glm::vec3(1.0f));

	// --- Create mesh with 0.25x scale transform ---
	res.mesh = resources.Load<Mesh>();
	res.mesh->o_mesh = meshData;
	res.mesh->o_material = res.material;
	res.mesh->SetScale(glm::vec3(0.25f));

	// Register mesh in scene so GeometryPass can iterate mesh_list
	res.scene = std::make_unique<Scene>();
	res.scene->UseMesh(res.mesh);

	// --- Create camera (pos 0,-5,2, target origin, 60° FOV, Z-up) ---
	res.camera = resources.Load<Camera>(width, height, 60.0f, 0.1f, 100.0f);
	res.camera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
	res.camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	// --- Create point light (pos 2,2,2, power 10, radius 10, white) ---
	res.light = resources.Load<Light>(LightType::POINTLIGHT, 10.0f, glm::vec3(1.0f));
	res.light->SetPosition(glm::vec3(2.0f, 2.0f, 2.0f));
	res.light->light_radius = 10.0f;

	NEURUS_LOG("[BuildDeferredScene] Loaded sphere scene from " << meshPath);

	return res;
}

} // namespace test
} // namespace neurus
