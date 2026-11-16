#pragma once

#include "render/SharedQueue.h"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace neurus {

/**
 * @brief Manages the Vulkan instance, physical device, and logical device.
 *
 * Three-phase construction so the instance is never moved while a surface
 * refers to its dispatcher:
 *   1. VulkanContext(std::move(instance)) - stores the instance
 *   2. InitDevice() - selects a physical device, creates the logical device
 *   3. InitQueue(surface) - validates presentation support, gets the queue
 */
class VulkanContext
{
public:
	/**
	 * @brief What the instance and device are created for.
	 *
	 * A windowed application needs both platform surface extensions and a
	 * swapchain-capable device. A headless renderer needs neither: it draws into
	 * its own attachments, so the instance carries no surface extension and
	 * VK_KHR_swapchain is not requested.
	 */
	struct Options
	{
		bool surfaceExtensions = true;
		bool swapchain = true;
	};

	/** @brief Creates a Vulkan instance with the platform's surface extensions. */
	static vk::raii::Instance CreateInstance();

	/** @brief Creates a Vulkan instance with the extensions @p options asks for. */
	static vk::raii::Instance CreateInstance(const Options& options);

	/** @brief Takes ownership of the instance (from CreateInstance). */
	explicit VulkanContext(vk::raii::Instance&& instance);

	/** @brief Selects physical device and creates a swapchain-capable logical device. */
	void InitDevice();

	/** @brief Selects physical device and creates the logical device @p options asks for. */
	void InitDevice(const Options& options);

	/** @brief Validates queue supports presentation to the surface and gets the queue handle. */
	void InitQueue(const vk::raii::SurfaceKHR& surface);

	/** @brief Gets the graphics queue without a presentation check (headless). */
	void InitQueue();

	~VulkanContext();

	VulkanContext(const VulkanContext&) = delete;
	VulkanContext& operator=(const VulkanContext&) = delete;
	VulkanContext(VulkanContext&&) noexcept = default;
	VulkanContext& operator=(VulkanContext&&) noexcept = default;

	const vk::raii::Instance& instance() const { return *ctx_instance; }
	const vk::raii::Device& device() const { return *ctx_device; }
	const vk::raii::PhysicalDevice& physicalDevice() const { return ctx_physicalDevices[ctx_selectedDeviceIndex]; }
	uint32_t graphicsQueueFamily() const { return ctx_graphicsQueueFamily; }
	/** @brief Graphics queue; valid after InitQueue(). */
	SharedQueue& graphicsQueue() const { return *ctx_graphicsQueue; }
	uint32_t transferQueueFamily() const { return ctx_transferQueueFamily; }
	/** @brief Transfer queue; valid after InitDevice(). */
	SharedQueue& transferQueue() const { return *ctx_transferQueue; }
	const std::string& gpuName() const { return ctx_gpuName; }

	/**
	 * @brief Core device features actually enabled at device creation.
	 *
	 * InitDevice() enables each optional feature only if the physical device
	 * reports it: requesting an unsupported feature makes vkCreateDevice fail
	 * with VK_ERROR_FEATURE_NOT_PRESENT. Consult this before relying on a
	 * feature-gated pipeline state.
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
	uint32_t ctx_transferQueueFamily = 0;

	// Declared after ctx_device so the wrappers - which borrow it - are destroyed
	// first. Each owned wrapper holds the one mutex every submit through that
	// queue must share, so a queue handle must never get two of them.
	std::unique_ptr<SharedQueue> ctx_transferQueueOwned;  // created in InitDevice
	std::unique_ptr<SharedQueue> ctx_graphicsQueueOwned;  // null when aliased
	SharedQueue* ctx_transferQueue = nullptr;
	SharedQueue* ctx_graphicsQueue = nullptr;

	std::string ctx_gpuName;
	vk::PhysicalDeviceFeatures ctx_enabledFeatures;
};

} // namespace neurus
