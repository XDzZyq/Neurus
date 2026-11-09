/**
 * @file TestCornellBox.h
 * @brief Header-only helper for loading the Cornell Box scene in GPU tests.
 *
 * Provides LoadCornellBox() which loads 6 OBJ meshes (back_wall, left_wall,
 * right_wall, left_box, right_box, light_up), registers them in a Scene
 * for auto-iteration by GeometryPass and ShadowDepthPass.
 *
 * Usage:
 * @code
 *   auto cb = LoadCornellBox(device, pd, queue, qfi);
 *   // Meshes registered in cb.scene.mesh_list — passes iterate automatically
 * @endcode
 *
 * The Cornell Box occupies approximately [-1, 1] in all axes, making it
 * suitable for SSAO testing (corners + crevices produce visible occlusion).
 */

#pragma once

#include "core/Log.h"
#include "scene/Material.h"
#include "scene/Scene.h"
#include "scene/Mesh.h"

#include "asset/data/MeshData.h"

#include "scene/Camera.h"
#include "scene/Light.h"

#include <glm/gtc/matrix_transform.hpp>

#include <memory>
#include <string>
#include <vector>

namespace neurus {

class SharedQueue;

namespace test {

/**
 * @brief Aggregated result of loading the Cornell Box scene.
 *
 * Meshes are registered in scene.mesh_list for automatic iteration by
 * GeometryPass and ShadowDepthPass.  GPU buffers are lazily created
 * via RenderCache::GetMeshGPU() on first access.
 */
struct CornellBoxResources
{
	std::shared_ptr<Scene> scene;

	std::shared_ptr<Camera> camera;
	std::shared_ptr<Light>  light;
};

/**
 * @brief Loads the Cornell Box scene from OBJ files and uploads to GPU.
 *
 * Loads 6 OBJ meshes with their respective material colours:
 *   back_wall    — white (1.0, 1.0, 1.0)
 *   left_wall    — red   (1.0, 0.0, 0.0)
 *   right_wall   — green (0.0, 1.0, 0.0)
 *   left_box     — white (1.0, 1.0, 1.0)
 *   right_box    — white (1.0, 1.0, 1.0)
 *   light_up     — emissive white (used as geometry)
 *
 * Creates a default camera at (-2.5, 1, 0) looking at (0, 1, 0) and a single
 * point light at (-0.3, 1.0, 0.8) with warm white colour (Z-up convention).
 *
 * @note All buffers use device-local memory. A staging upload is performed
 *       synchronously on the provided graphics queue.
 * @note The OBJ files must be relative to ResolveAssetPath("res/obj/cornellbox/").
 *
 * @param device           Logical device.
 * @param physicalDevice   Physical device (for memory properties).
 * @param graphicsQueue    Graphics queue for staging uploads.
 * @param queueFamilyIndex Queue family index for GPUBuffer creation.
 * @param basePath         Base path for OBJ files (default: "res/obj/cornellbox/").
 * @return Fully populated CornellBoxResources (move-only due to unique_ptr members).
 */
inline CornellBoxResources LoadCornellBox(
	const vk::raii::Device& /*device*/,
	const vk::raii::PhysicalDevice& /*physicalDevice*/,
	neurus::SharedQueue& /*graphicsQueue*/,
	uint32_t /*queueFamilyIndex*/,
	const std::string& basePath = "res/obj/cornellbox/")
{
	CornellBoxResources res;
	res.scene = std::make_shared<Scene>();

	// --- OBJ file names ---
	const std::vector<const char*> filenames = {
		"back_wall.obj", "left_wall.obj", "right_wall.obj",
		"left_box.obj", "right_box.obj", "light_up.obj",
	};

	for (const auto& filename : filenames)
	{
		// --- Load OBJ (try multiple relative paths) ---
		const std::string relPath = basePath + filename;
		std::string objPath = relPath;
		{
			// CTest runs from build/debug/test/ or build/debug/
			std::ifstream f(std::string("../../../") + relPath);
			if (f.good()) { objPath = std::string("../../../") + relPath; }
			else { f = std::ifstream(std::string("../../") + relPath); if (f.good()) objPath = std::string("../../") + relPath; }
		}

		auto meshData = std::make_shared<MeshData>();
		if (!meshData->LoadObj(objPath))
		{
			NEURUS_ERR("LoadCornellBox: Failed to load " << objPath);
			continue;
		}

		const auto& rawMesh = meshData->GetMeshData();
		const size_t vertexCount = rawMesh.dataArray.size() / 14;
		const size_t indexCount = rawMesh.indexArray.size();

		if (vertexCount == 0 || indexCount == 0)
		{
			NEURUS_ERR("LoadCornellBox: Empty geometry in " << filename);
			continue;
		}

		// --- Create Mesh and register in scene ---
		// GPU buffers created lazily by RenderCache::GetMeshGPU() on first use.
		auto mesh = std::make_shared<Mesh>();
		mesh->o_name = std::string("CornellBox_") + filename;
		mesh->o_mesh = meshData;
		res.scene->UseMesh(mesh);
	}

	// --- Create default camera ---
	// Positioned to view the interior of the box from slightly above.
	res.camera = std::make_shared<Camera>(
		static_cast<float>(256),  // width (placeholder, caller adjusts)
		static_cast<float>(256),  // height
		60.0f, 0.1f, 100.0f);
	res.camera->SetPosition(glm::vec3(-2.5f, 0.0f, 1.0f));  // Z-up: left side, cube mid-height Z=1
	res.camera->SetTarPos(glm::vec3(0.0f, 0.0f, 1.0f));   // looking into cube at mid-height
	res.camera->SetRotation(glm::vec3(0.0f, 0.0f, 0.0f));  // no explicit rotation, lookAt handles it

	// --- Create a point light near the ceiling ---
	res.light = std::make_shared<Light>(LightType::POINTLIGHT, 30.0f,
	                                    glm::vec3(1.0f, 0.95f, 0.8f));
	res.light->SetPosition(glm::vec3(-0.3f, 1.0f, 0.8f));  // Z-up: near ceiling

	NEURUS_LOG("[LoadCornellBox] Loaded 6 meshes from " << basePath);

	return res;
}

} // namespace test
} // namespace neurus
