/**
 * @file TestSimpleShadow.h
 * @brief Header-only helper for a simple shadow-map test scene: Cube + Plane.
 *
 * Provides LoadSimpleShadow() which procedurally generates a unit cube
 * (centered at origin, positioned at (0,0,3) via SetPosition) and a large
 * ground plane (z=0, [-10,10] in XY),
 * uploads them to GPU via Scene/Mesh/Light, and returns render items
 * suitable for ShadowDepthPass.
 *
 * Usage:
 * @code
 *   auto scene = LoadSimpleShadow(device, pd, queue, qfi);
 *   // Light position is read from ctx.scene->light_list at Record() time.
 *   m_shadowPass->Record(cmd, ctx);  // uses scene.renderItems internally
 * @endcode
 *
 * Geometry is 100% procedural via OBJ strings — no OBJ files needed.
 */

#pragma once

#include "core/Log.h"
#include "scene/Scene.h"
#include "scene/Mesh.h"
#include "scene/Light.h"
#include "scene/Camera.h"
#include "asset/data/MeshData.h"
#include "render/buffers/VertexBuffer.h"
#include "render/buffers/IndexBuffer.h"
#include "shared/TestShadowPrimitives.h"

#include <vulkan/vulkan_raii.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace neurus {
namespace test {

/**
 * @brief Aggregated result of building the simple shadow test scene.
 *
 * The Scene owns all GPU resources (meshes hold VertexBuffer/IndexBuffer
 * internally).  renderItems provides a flat list for direct use with
 * ShadowDepthPass.
 */
struct SimpleShadowResources
{
	std::shared_ptr<Scene> scene;
	glm::vec3               cubePos;        // World-space position of the cube (default: (0,0,3))
};

/**
 * @brief Builds the simple shadow-map test scene procedurally.
 *
 * Generates:
 *   - A unit cube (12 triangles, 8 vertices, 36 indices) centred at origin
 *     covering [-0.5, +0.5]^3, positioned at (0,0,3) via SetPosition (Z-up).
 *   - A ground-plane quad (2 triangles, 4 vertices, 6 indices) at z=0,
 *     spanning [-10,10] in XY, facing +Z.
 *
 * The plane uses identity; the cube's model matrix shifts it to (0, 0, 3).
 * The light is positioned at (0, 0, 6) — above the cube, casting
 * shadows downward onto the plane (Z-up convention).
 *
 * @note All buffers use device-local memory.  A staging upload is performed
 *       synchronously on the provided graphics queue via Mesh::UploadToGPU().
 *
 * @param device           Logical device.
 * @param physicalDevice   Physical device (for memory properties).
 * @param graphicsQueue    Graphics queue for staging uploads.
 * @param queueFamilyIndex Queue family index for GPUBuffer creation.
 * @return Fully populated SimpleShadowResources with scene, renderItems,
 *         shadowCamera, and cubePos.
 */
inline SimpleShadowResources LoadSimpleShadow(
	const vk::raii::Device& device,
	const vk::raii::PhysicalDevice& physicalDevice,
	vk::Queue graphicsQueue,
	uint32_t queueFamilyIndex)
{
	SimpleShadowResources res;
	res.scene = std::make_shared<Scene>();

	// ===================================================================
	//  1. Cube: unit cube centred at origin [-0.5, +0.5]^3
	//     8 unique vertices, 12 triangles (36 indices)
	// ===================================================================

	{
		auto cubeMeshData = MakeShadowTestCubeMeshData("[LoadSimpleShadow]");
		if (!cubeMeshData) return res;

		auto cubeMesh = std::make_shared<Mesh>();
		cubeMesh->o_name = "SimpleShadowCube";
		cubeMesh->o_mesh = cubeMeshData;
		cubeMesh->SetPosition(glm::vec3(0.0f, 0.0f, 3.0f));  // raise cube above plane (Z-up)

		res.scene->UseMesh(cubeMesh);
		res.cubePos = cubeMesh->GetPosition();
	}

	// ===================================================================
	//  2. Plane: large quad at z=0, [-10,10] in XY, facing +Z
	//     4 vertices, 2 triangles (6 indices)
	// ===================================================================

	{
		auto planeMeshData = MakeShadowTestPlaneMeshData("[LoadSimpleShadow]");
		if (!planeMeshData) return res;

		auto planeMesh = std::make_shared<Mesh>();
		planeMesh->o_name = "SimpleShadowPlane";
		planeMesh->o_mesh = planeMeshData;

		res.scene->UseMesh(planeMesh);
	}

	// ===================================================================
	//  3. Point light at (0, 0, 6) — above the cube (z=3), casting shadows
	//     downward onto the plane (z=0). Z-up convention.
	// ===================================================================

	{
		auto light = std::make_shared<Light>(LightType::POINTLIGHT, 10.0f, glm::vec3(1.0f));
		light->o_name = "SimpleShadowLight";
		light->SetPosition(glm::vec3(0.0f, 0.0f, 6.0f));
		res.scene->UseLight(light);
	}

	// ===================================================================
	//  4. Camera between plane (z=0) and cube (z=3), facing the plane.
	//     75deg FOV at z=2 covers +/-1.534 at plane -- full +/-1.2 shadow
	//     captured with 28% margin for extra no-shadow region.
	// ===================================================================

	{
		auto cam = std::make_shared<Camera>(256.0f, 256.0f, 75.0f, 0.1f, 100.0f);
		cam->o_name = "SimpleShadowCamera";
		cam->SetPosition(glm::vec3(0.0f, 0.001f, 2.0f));  // small Y offset avoids degenerate lookAt
		cam->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
		res.scene->UseCamera(cam);
		// Registration no longer implies activation; the fixture renders through
		// this camera, so it activates it explicitly.
		res.scene->ActivateCamera(cam->GetObjectID());
	}

	NEURUS_LOG("[LoadSimpleShadow] Built 2 meshes (cube + plane) + 1 point light + 1 camera");

	return res;
}

} // namespace test
} // namespace neurus
