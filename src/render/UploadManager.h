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
class ImageData;         // asset/data/ImageData.h
class Environment;       // scene/Environment.h
class Light;             // scene/Light.h
class Scene;             // scene/Scene.h
class Shader;             // render/shaders/Shader.h
class SharedQueue;        // render/SharedQueue.h
class BufferLayout;      // render/buffers/BufferLayout.h
class RenderCache;       // render/RenderCache.h
struct MeshGPU;          // render/resources/MeshGPU.h
struct EnvironmentGPU;   // render/resources/EnvironmentGPU.h
struct LightGPU;         // render/resources/LightGPU.h
struct Pipeline;         // render/Pipeline.h
enum LightType : int;    // scene/Light.h

	/**
	 * @brief CPU-to-GPU upload service.
	 *
	 * Owns a separate command pool and IBLPass for asynchronous uploads.
	 * Stateless — each Upload*() method performs one-shot construction and
	 * returns the GPU resource by value.
	 *
	 * @param device               Logical device.
	 * @param physicalDevice       Physical device (for buffer memory queries).
	 * @param transferQueue        Upload queue for staging copies and IBL dispatches,
	 *                             shared with every other submitter; its family must
	 *                             support transfer and compute.
	 * @param transferQueueFamily  Family index of @p transferQueue.
	 */
	class UploadManager
	{
	public:
		UploadManager(const vk::raii::Device& device,
		              const vk::raii::PhysicalDevice& physicalDevice,
		              SharedQueue& transferQueue,
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
	 * The geometry half of UploadMesh(), for callers that hold a MeshData
	 * without a Mesh.
	 *
	 * @param meshData CPU-side mesh (14-float interleaved vertices + uint32 indices).
	 * @return Device-local MeshGPU, or an empty MeshGPU if the data is empty.
	 */
	MeshGPU UploadMeshData(const MeshData& meshData);

	/** @brief Upload environment map to GPU. Generates diffuse + specular cubemaps. */
	EnvironmentGPU UploadEnvironment(const Environment& env);

	/**
	 * @brief Upload an equirect to GPU and generate its IBL cubemaps.
	 *
	 * The Scene-free half of UploadEnvironment(), for the request executor,
	 * which holds no Environment. A null or invalid @p equirect selects the
	 * pink-purple procedural fallback, exactly as a missing source file does.
	 * The convolution dispatches on this manager's own upload queue, which is
	 * why that queue's family must support compute (see
	 * VulkanContext::findTransferQueueFamily).
	 *
	 * @param equirect Equirectangular source, or null for the fallback.
	 * @return Diffuse + specular cubemap EnvironmentGPU.
	 */
	EnvironmentGPU UploadEnvironment(std::shared_ptr<const ImageData> equirect);

	/** @brief Upload light shadow resources to GPU. Creates shadow depth map. */
	LightGPU UploadLight(const Light& light);

	/**
	 * @brief Allocate a light's shadow maps from its uid and type alone.
	 *
	 * The data UploadLight(const Light&) actually reads: the uid names the
	 * images and the type sizes them. For the request executor, which holds no
	 * Light. A SUNLIGHT gets a 2D depth map and no colour map; point/spot lights
	 * get a depth cubemap plus a colour cubemap.
	 *
	 * @param uid   Light UID (names the GPU images).
	 * @param type  Light type (sizes and shapes the shadow maps).
	 * @return Constructed LightGPU, or an empty LightGPU for NONELIGHT.
	 */
	LightGPU UploadLight(int uid, LightType type);

	/**
	 * @brief Convert a map of lights to GPU structs (variant-based).
	 *
	 * Pure CPU conversion - no GPU operations, and no member state, so static:
	 * the Editor calls it without holding an UploadManager. Each struct gets
	 * shadowMapIndex = -1; the caller assigns real shadow indices.
	 *
	 * @param lights  Map of light UID to shared pointers to Light objects.
	 * @return Map: light UID -> variant<PointLightStruct, SunLightStruct, SpotLightStruct>.
	 */
	static std::unordered_map<int, std::variant<PointLightStruct, SunLightStruct, SpotLightStruct>>
	UploadLighting(const std::unordered_map<int, std::shared_ptr<Light>>& lights);

	/**
	 * @brief Convert a single light to its GPU struct (variant-based).
	 *
	 * Pure CPU conversion - no GPU operations and no member state, so static.
	 * Returns the appropriate GPU struct with shadowMapIndex = -1, or a default
	 * struct for NONELIGHT.
	 *
	 * @param light  The light to convert.
	 * @return variant<PointLightStruct, SunLightStruct, SpotLightStruct>.
	 */
	static std::variant<PointLightStruct, SunLightStruct, SpotLightStruct>
	UploadLighting(const Light& light);

	/**
	 * @brief Creates a LightingCache (light SSBO storage) using the transfer queue.
	 *
	 * @param device          Logical device.
	 * @param physicalDevice  Physical device (for buffer memory queries).
	 * @return Fully constructed LightingCache.
	 */
	std::unique_ptr<LightingCache> CreateLightingCache(const vk::raii::Device& device,
	                                               const vk::raii::PhysicalDevice& physicalDevice);


private:
	const vk::raii::Device* um_device = nullptr;
	const vk::raii::PhysicalDevice* um_physicalDevice = nullptr;
	vk::raii::CommandPool um_commandPool{ nullptr };
	SharedQueue* um_transferQueue = nullptr;
	uint32_t um_transferQueueFamily = 0;

	/** @brief Internal IBL pass for environment map generation on upload. */
	std::unique_ptr<class IBLPass> um_iblPass;
};

} // namespace neurus
