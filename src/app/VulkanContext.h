#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace neurus {

class PlatformSurface;

/**
 * @brief Manages the Vulkan instance, physical device, and logical device.
 *
 * Three-phase construction to avoid moving the instance while the surface
 * holds a reference to its dispatcher:
 *   1. VulkanContext(std::move(instance)) - stores the instance (no move during surface lifetime)
 *   2. InitDevice() - selects physical device, creates logical device with graphics queue
 *   3. InitQueue(surface) - validates queue supports presentation, gets queue handle
 */
class VulkanContext
{
public:
	/** @brief Creates a Vulkan instance with platform-provided extensions. */
	static vk::raii::Instance CreateInstance(const PlatformSurface& platform);

	/** @brief Takes ownership of the instance (from CreateInstance). */
	explicit VulkanContext(vk::raii::Instance&& instance);

	/** @brief Selects physical device and creates logical device with a graphics queue family. */
	void InitDevice();

	/** @brief Validates queue supports presentation to the surface and gets the queue handle. */
	void InitQueue(const vk::raii::SurfaceKHR& surface);

	~VulkanContext();

	VulkanContext(const VulkanContext&) = delete;
	VulkanContext& operator=(const VulkanContext&) = delete;
	VulkanContext(VulkanContext&&) noexcept = default;
	VulkanContext& operator=(VulkanContext&&) noexcept = default;

	const vk::raii::Instance& instance() const { return *ctx_instance; }
	const vk::raii::Device& device() const { return *ctx_device; }
	const vk::raii::PhysicalDevice& physicalDevice() const { return ctx_physicalDevices[ctx_selectedDeviceIndex]; }
	uint32_t graphicsQueueFamily() const { return ctx_graphicsQueueFamily; }
	vk::Queue graphicsQueue() const { return ctx_graphicsQueue; }
	uint32_t transferQueueFamily() const { return ctx_transferQueueFamily; }
	vk::Queue transferQueue() const { return ctx_transferQueue; }
	const std::string& gpuName() const { return ctx_gpuName; }

	/**
	 * @brief Core device features actually enabled at device creation.
	 *
	 * Requesting an unsupported feature makes vkCreateDevice fail with
	 * VK_ERROR_FEATURE_NOT_PRESENT, so InitDevice() enables each optional
	 * feature only if the physical device reports it. Passes must consult
	 * this before using a feature-gated pipeline state (e.g. DebugPass
	 * checks fillModeNonSolid before building its wireframe pipeline).
	 */
	const vk::PhysicalDeviceFeatures& enabledFeatures() const { return ctx_enabledFeatures; }

private:
	static vk::raii::Instance createInstanceInternal();
	uint32_t selectPhysicalDeviceIndex();
	uint32_t findGraphicsQueueFamily();
	uint32_t findGraphicsQueueFamilyWithPresent(const vk::raii::SurfaceKHR& surface);
	uint32_t findTransferQueueFamily();

	std::unique_ptr<vk::raii::Instance> ctx_instance;
	vk::raii::PhysicalDevices ctx_physicalDevices = nullptr;
	uint32_t ctx_selectedDeviceIndex = 0;
	std::unique_ptr<vk::raii::Device> ctx_device;

	uint32_t ctx_graphicsQueueFamily = 0;
	vk::Queue ctx_graphicsQueue = nullptr;

	uint32_t ctx_transferQueueFamily = 0;
	vk::Queue ctx_transferQueue = nullptr;

	std::string ctx_gpuName;
	vk::PhysicalDeviceFeatures ctx_enabledFeatures;
};

} // namespace neurus
