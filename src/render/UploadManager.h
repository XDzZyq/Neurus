#pragma once

#include <vulkan/vulkan_raii.hpp>

#include "resources/LightingCache.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

namespace neurus {

// Forward declarations
class IBLPass;
class Mesh;              // scene/Mesh.h
class MeshData;          // asset/data/MeshData.h
class Environment;       // scene/Environment.h
class Light;             // scene/Light.h
class Scene;             // scene/Scene.h
class Shader;             // render/shaders/Shader.h
class BufferLayout;      // render/buffers/BufferLayout.h
class RenderCache;       // render/RenderCache.h
struct MeshGPU;          // render/resources/MeshGPU.h
struct EnvironmentGPU;   // render/resources/EnvironmentGPU.h
struct LightGPU;         // render/resources/LightGPU.h
struct Pipeline;         // render/Pipeline.h

	/**
	 * @brief CPU-to-GPU upload service.
	 *
	 * Owns a separate command pool and IBLPass for asynchronous uploads.
	 * Stateless — each Upload*() method performs one-shot construction and
	 * returns the GPU resource by value.
	 *
	 * @param device               Logical device.
	 * @param physicalDevice       Physical device (for buffer memory queries).
	 * @param transferQueue        Transfer queue for staging uploads.
	 * @param transferQueueFamily  Transfer queue family index.
	 */
	class UploadManager
	{
	public:
		UploadManager(const vk::raii::Device& device,
		              const vk::raii::PhysicalDevice& physicalDevice,
		              vk::Queue transferQueue,
		              uint32_t transferQueueFamily);
	~UploadManager();

	// Non-copyable
	UploadManager(const UploadManager&) = delete;
	UploadManager& operator=(const UploadManager&) = delete;

	// Non-movable (owns IBLPass internally via unique_ptr)
	UploadManager(UploadManager&&) = delete;
	UploadManager& operator=(UploadManager&&) = delete;

	/** @brief Upload mesh geometry to GPU. Returns device-local buffers by value. */
	MeshGPU UploadMesh(const Mesh& mesh);

	/**
	 * @brief Upload raw CPU mesh geometry to GPU.
	 *
	 * The geometry half of UploadMesh(), split out so scene objects that carry a
	 * MeshData without being a Mesh (notably DebugMesh, which derives from
	 * ObjectID + Transform3D) can produce a MeshGPU through the same path
	 * instead of duplicating the vertex-stripping logic.
	 *
	 * @param meshData CPU-side mesh (14-float interleaved vertices + uint32 indices).
	 * @return Device-local MeshGPU, or an empty MeshGPU if the data is empty.
	 */
	MeshGPU UploadMeshData(const MeshData& meshData);

	/** @brief Upload environment map to GPU. Generates diffuse + specular cubemaps. */
	EnvironmentGPU UploadEnvironment(const Environment& env,
	                                 vk::Queue graphicsQueue,
	                                 uint32_t graphicsQueueFamily);

	/** @brief Upload light shadow resources to GPU. Creates shadow depth map. */
	LightGPU UploadLight(const Light& light);

	/**
	 * @brief Convert a map of lights to GPU structs (variant-based).
	 *
	 * Pure CPU conversion — no GPU operations.  Iterates the light map,
	 * converts each light to PointLightStruct / SunLightStruct /
	 * SpotLightStruct with shadowMapIndex set to -1, and returns a map
	 * keyed by light UID.  The caller (RenderCache::UpdateLighting)
	 * assigns real shadow indices.
	 *
	 * @param lights  Map of light UID to shared pointers to Light objects.
	 * @return Map: light UID → variant<PointLightStruct, SunLightStruct, SpotLightStruct>.
	 */
	std::unordered_map<int, std::variant<PointLightStruct, SunLightStruct, SpotLightStruct>>
	UploadLighting(const std::unordered_map<int, std::shared_ptr<Light>>& lights);

	/**
	 * @brief Convert a single light to its GPU struct (variant-based).
	 *
	 * Pure CPU conversion — no GPU operations.  Returns a variant containing
	 * the appropriate GPU struct with shadowMapIndex set to -1.
	 * Returns an empty variant (default-initialized PointLightStruct) for
	 * LightType::NONELIGHT.
	 *
	 * @param light  The light to convert.
	 * @return variant<PointLightStruct, SunLightStruct, SpotLightStruct>.
	 */
	std::variant<PointLightStruct, SunLightStruct, SpotLightStruct>
	UploadLighting(const Light& light);

	/** @brief Wait for all pending upload operations to complete. */
	void WaitIdle();

	/**
	 * @brief Creates a LightingCache (light SSBO storage) using the transfer queue.
	 *
	 * The LightingCache holds device-local SSBOs for point and sun light data.
	 * Staging uploads use the transfer queue for true async operation.
	 *
	 * @param device          Logical device.
	 * @param physicalDevice  Physical device (for buffer memory queries).
	 * @return Fully constructed LightingCache, ready to be passed to RenderCache::SetLightingCache().
	 */
	std::unique_ptr<LightingCache> CreateLightingCache(const vk::raii::Device& device,
	                                               const vk::raii::PhysicalDevice& physicalDevice);


private:
	const vk::raii::Device* um_device = nullptr;
	const vk::raii::PhysicalDevice* um_physicalDevice = nullptr;
	vk::raii::CommandPool um_commandPool{ nullptr };
	vk::Queue um_transferQueue = nullptr;
	uint32_t um_transferQueueFamily = 0;

	/** @brief Internal IBL pass for environment map generation on upload. */
	std::unique_ptr<class IBLPass> um_iblPass;
};

} // namespace neurus
