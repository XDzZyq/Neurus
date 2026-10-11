#pragma once

#include "Buffer.h"
#include "StagingBuffer.h"

#include <vulkan/vulkan_raii.hpp>

#include <memory>

namespace neurus {

/**
 * @brief Device-local GPU buffer with staging-backed Map/Unmap.
 *
 * The buffer is allocated in DEVICE_LOCAL memory for optimal GPU access.
 * Map() and Unmap() transparently use an internal staging buffer: Map()
 * returns a writable pointer to it, and Unmap() copies its content to the
 * device-local buffer on the graphics queue.
 *
 * Usage flags are automatically extended with eTransferDst to support the
 * staging transfer.
 */
class GPUBuffer : public Buffer
{
public:
	/**
	 * @brief Creates a device-local buffer with staging support.
	 *
	 * @param device           Borrowed logical device (outlives this buffer).
	 * @param physicalDevice   Borrowed physical device for memory queries.
	 * @param queue            Borrowed graphics queue for staging transfers.
	 * @param queueFamilyIndex Queue family index for temp command pool creation.
	 * @param size             Buffer size in bytes.
	 * @param usageFlags       Buffer usage flags. eTransferDst is added automatically.
	 * @param debugName        Optional debug name.
	 */
	GPUBuffer(const vk::raii::Device& device,
	          const vk::raii::PhysicalDevice& physicalDevice,
	          vk::Queue queue,
	          uint32_t queueFamilyIndex,
	          vk::DeviceSize size,
	          vk::BufferUsageFlags usageFlags,
	          const char* debugName = nullptr);

	// Inherits non-copyable / movable from Buffer — no need to re-delete

	/**
	 * @brief Uploads host data using the Map/Unmap staging pattern.
	 *
	 * @param data Pointer to source data on the host.
	 * @param size Number of bytes to upload (must not exceed buffer size).
	 */
	void Upload(const void* data, vk::DeviceSize size) override;

	/**
	 * @brief Maps the internal staging buffer for CPU write access.
	 *
	 * The caller writes data through the returned pointer, then calls
	 * Unmap() to trigger the GPU transfer.
	 *
	 * @return Writable pointer to the staging buffer memory.
	 * @todo Read mapping
	 * @todo Offset + Size for partial mapping/
	 * @todo Safer API
	 */
	void* Map() override;

	/**
	 * @brief Transfers the staging buffer content to the device-local buffer.
	 *
	 * Records a one-shot vkCmdCopyBuffer, submits it to the graphics queue
	 * and waits for completion.
	 */
	void Unmap() override;

private:
	vk::Queue b_queue = nullptr;
	uint32_t b_queueFamilyIndex = 0;

	std::unique_ptr<StagingBuffer> b_staging;
};

} // namespace neurus
