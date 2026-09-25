/**
 * @file TestMultiShadow.h
 * @brief Header-only parametrized cube-on-plane scene builder with N lights.
 *
 * Provides LoadMultiShadow() which procedurally generates a unit cube
 * (centered at origin, positioned at (0,0,0)) and a large
 * ground plane (z=0, [-10,10] in XY), with N shadow-casting lights.
 *
 * Usage:
 * @code
 *   auto res = LoadMultiShadow(device, pd, queue, qfi);                  // 3 point lights (default)
 *   auto res = LoadMultiShadow(device, pd, queue, qfi, 4);               // 4 point lights
 *   auto res = LoadMultiShadow(device, pd, queue, qfi, 3, LightType::SUNLIGHT); // 3 sun lights
 * @endcode
 *
 * Light placement:
 *   - POINTLIGHT (default): N point lights on a ring (z=2, radius=2),
 *     each casting shadows downward onto the plane.
 *   - SUNLIGHT: N directional lights placed on the same ring, each
 *     rotated to point toward the cube centre (origin).  The rotation
 *     maps the default forward direction (0,1,0) to the vector from
 *     the ring position toward origin.
 *
 * Geometry is 100% procedural via OBJ strings -- no OBJ files needed.
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
 * @brief Aggregated result of building the multi-shadow test scene.
 *
 * The Scene owns all GPU resources (meshes hold VertexBuffer/IndexBuffer
 * internally).  renderItems provides a flat list for direct use with
 * ShadowDepthPass.  lightUIDs provides the unique IDs of all created lights
 * for per-light shadow map lookup.
 */
struct MultiShadowResources
{
	std::shared_ptr<Scene> scene;
	std::vector<int> lightUIDs;
};

/**
 * @brief Builds the multi-shadow test scene procedurally with N lights.
 *
 * Generates:
 *   - A unit cube (12 triangles, 8 vertices, 36 indices) centred at origin
 *     covering [-0.5, +0.5]^3, positioned at (0,0,0) (resting on the plane).
 *   - A ground-plane quad (2 triangles, 4 vertices, 6 indices) at z=0,
 *     spanning [-10,10] in XY, facing +Z.
 *
 * The cube and plane both use identity model matrices (cube geometry at origin,
 * plane geometry at z=0).
 *
 * Lights: for POINTLIGHT (default), lights are placed on a circle of radius 2
 * at z=2.  For SUNLIGHT, lights are also placed on the circle at the same
 * positions but configured as directional sun lights whose forward direction
 * points toward the cube centre (origin).
 *
 * For both types, lights are distributed evenly around the circle at angles
 * 0deg, 360deg/N, etc.  All lights have power=5, colour=white, and
 * shadow=true.
 *
 * @note All buffers use device-local memory.  A staging upload is performed
 *       synchronously on the provided graphics queue via Mesh::UploadToGPU().
 *
 * @param device           Logical device.
 * @param physicalDevice   Physical device (for memory properties).
 * @param graphicsQueue    Graphics queue for staging uploads.
 * @param queueFamilyIndex Queue family index for GPUBuffer creation.
 * @param numLights        Number of shadow-casting lights (default: 3).
 * @param lightType        Type of lights to create (default: POINTLIGHT).
 * @return Fully populated MultiShadowResources with scene, renderItems,
 *         and lightUIDs.
 */
inline MultiShadowResources LoadMultiShadow(
	const vk::raii::Device& device,
	const vk::raii::PhysicalDevice& physicalDevice,
	vk::Queue graphicsQueue,
	uint32_t queueFamilyIndex,
	int numLights = 3,
	LightType lightType = LightType::POINTLIGHT)
{
	MultiShadowResources res;
	res.scene = std::make_shared<Scene>();

	// ===================================================================
	//  1. Cube: unit cube centred at origin [-0.5, +0.5]^3
	//     8 unique vertices, 12 triangles (36 indices)
	// ===================================================================

	{
		auto cubeMeshData = MakeShadowTestCubeMeshData("[LoadMultiShadow]");
		if (!cubeMeshData) return res;

		auto cubeMesh = std::make_shared<Mesh>();
		cubeMesh->o_name = "MultiShadowCube";
		cubeMesh->o_mesh = cubeMeshData;
		cubeMesh->SetPosition(glm::vec3(0.0f, 0.0f, 0.5f));  // cube bottom at z=0, rests on plane

		res.scene->UseMesh(cubeMesh);
	}

	// ===================================================================
	//  2. Plane: large quad at z=0, [-10,10] in XY, facing +Z
	//     4 vertices, 2 triangles (6 indices)
	// ===================================================================

	{
		auto planeMeshData = MakeShadowTestPlaneMeshData("[LoadMultiShadow]");
		if (!planeMeshData) return res;

		auto planeMesh = std::make_shared<Mesh>();
		planeMesh->o_name = "MultiShadowPlane";
		planeMesh->o_mesh = planeMeshData;

		res.scene->UseMesh(planeMesh);
	}

	// ===================================================================
	//  3. N lights on a circle at z=2, radius r=2, all shadow-casting,
	//     power=5, colour=white.
	//
	//     POINTLIGHT: lights are above the cube (at z=2 vs cube at z=0)
	//       so shadows project downward onto the plane.
	//     SUNLIGHT: directional lights whose forward direction points
	//       from the ring position toward the cube centre (origin).
	//
	//     Lights are distributed evenly around the circle at angles
	//     0deg, 360deg/N, etc.  Camera at (0, 1, 3) provides a
	//     good view of the cube + plane + shadows (Z-up).
	// ===================================================================

	for (int i = 0; i < numLights; ++i)
	{
		const float radius = 2.0f;

		// Sun lights: cluster on the -Y half of the ring so all shadows
		// are cast toward +Y (visible from the camera at (0, 1, 3)).
		// Point lights: evenly distribute around the full ring (shadows
		// always cast downward onto the plane regardless of XY angle).
		float angleDeg;
		if (lightType == LightType::SUNLIGHT)
		{
			// 120° spread centered at 45°: angles -75°, 45°, 165° for 3 lights.
			// Center chosen so all 3 shadows are visible from the camera and
			// none is fully occluded by the cube.
			constexpr float sunSpread = 120.0f;  // degrees between adjacent lights
			constexpr float sunCenter = 45.0f;  // ring center angle
			angleDeg = sunCenter + (static_cast<float>(i) - (numLights - 1) / 2.0f) * sunSpread;
		}
		else
		{
			angleDeg = static_cast<float>(i) * 360.0f / static_cast<float>(numLights);
		}
		const float angle = glm::radians(angleDeg);
		glm::vec3 pos(radius * cos(angle), radius * sin(angle), 2.0f);

		if (lightType == LightType::SUNLIGHT)
		{
			// Directional light pointing from ring position toward origin.
			// Compute Euler angles (pitch, yaw, 0) that rotate the default
			// forward (0,1,0) to the direction from pos toward origin.
			const glm::vec3 dir = glm::normalize(glm::vec3(0.0f) - pos);
			const float pitchRad = asin(dir.z);
			// yaw = atan2(-dir.x, dir.y) inverts GetDirection():
			//   d.x = -cos(p) * sin(y)  →  sin(y) = -dir.x / cos(p)
			//   d.y =  cos(p) * cos(y)  →  cos(y) =  dir.y / cos(p)
			const float yawRad   = atan2(-dir.x, dir.y);

			auto light = std::make_shared<Light>(LightType::SUNLIGHT, 5.0f, glm::vec3(1.0f));
			light->o_name    = "MultiShadowSunLight_" + std::to_string(i);
			light->SetPower(0.5f);
			// m_rotation = (pitch=X, roll=Y, yaw=Z)
			light->SetRotation(glm::vec3(
				glm::degrees(pitchRad),
				0.0f,  // roll = 0
				glm::degrees(yawRad)));
			light->SetShadow(true);

			NEURUS_LOG("[LoadMultiShadow] Sunlight " << i << " pos=" << pos.x << ", " << pos.y << ", " << pos.z << " pitch=" << glm::degrees(pitchRad) << " yaw=" << glm::degrees(yawRad));

			const glm::vec3 updated_dir = light->GetDirection();
			NEURUS_LOG("[LoadMultiShadow] Sunlight " << i << " origin dir=" << dir.x << ", " << dir.y << ", " << dir.z << " updated=" << updated_dir.x << ", " << updated_dir.y << ", " << updated_dir.z);
			res.scene->UseLight(light);
			res.lightUIDs.push_back(light->GetObjectID());
		}
		else
		{
			auto light = std::make_shared<Light>(LightType::POINTLIGHT, 5.0f, glm::vec3(1.0f));
			light->o_name = "MultiShadowLight_" + std::to_string(i);

			light->SetPosition(pos);
			light->SetPower(3.0f);  // keep existing default for point lights
			light->SetRadius(0.5f);
			light->SetColor(glm::vec3(1.0f));
			light->SetShadow(true);

			res.scene->UseLight(light);
			res.lightUIDs.push_back(light->GetObjectID());
		}
	}

	// ===================================================================
	//  4. Camera at (0, 1, 3) looking at origin — provides a clear view
	//     of the cube(shadow caster) and the plane below with shadows.
	// ===================================================================

	{
		auto cam = std::make_shared<Camera>(256.0f, 256.0f, 75.0f, 0.1f, 100.0f);
		cam->o_name = "MultiShadowCamera";
		cam->SetPosition(glm::vec3(0.0f, 1.0f, 3.0f));  // looking down at origin from above (Z-up)
		cam->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
		res.scene->UseCamera(cam);
		// Registration no longer implies activation; the fixture renders through
		// this camera, so it activates it explicitly.
		res.scene->ActivateCamera(cam->GetObjectID());
	}

	NEURUS_LOG("[LoadMultiShadow] Built 2 meshes (cube + plane) + "
	           << numLights
	           << (lightType == LightType::SUNLIGHT ? " sun" : " point")
	           << " lights + 1 camera");

	return res;
}

} // namespace test
} // namespace neurus
