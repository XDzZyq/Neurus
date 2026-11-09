#pragma once

#include "GPUBuffer.h"

#include <vulkan/vulkan_raii.hpp>

namespace neurus {

class SharedQueue;

/**
 * @brief Device-local index buffer with staging-backed upload.
 *
 * Inherits the buffer lifecycle (device-local allocation, staging-backed
 * Map/Unmap/Upload) from GPUBuffer and specializes the usage to
 * INDEX_BUFFER.
 *
 * @note Indices are always uint32_t (VK_INDEX_TYPE_UINT32), supporting up to
 *       4B indices.
 */
class IndexBuffer : public GPUBuffer
{
public:
	/**
	 * @brief Creates a device-local index buffer and uploads index data.
	 *
	 * @param device           Borrowed logical device (outlives this buffer).
	 * @param physicalDevice   Borrowed physical device for memory queries.
	 * @param queue            Borrowed graphics queue for staging submits;
	 *                         the upload waits its own submit.
	 * @param queueFamilyIndex Queue family index for temp command pool creation.
	 * @param data             Pointer to host index data (uint32_t array).
	 * @param size             Total byte size of index data (indexCount * sizeof(uint32_t)).
	 * @param indexCount       Number of indices in the buffer.
	 * @param debugName        Optional debug name (set via VK_EXT_debug_utils in Debug builds).
	 */
	IndexBuffer(const vk::raii::Device& device,
	            const vk::raii::PhysicalDevice& physicalDevice,
	            SharedQueue& queue,
	            uint32_t queueFamilyIndex,
	            const uint32_t* data,
	            vk::DeviceSize size,
	            uint32_t indexCount,
	            const char* debugName = nullptr);

	// Non-copyable - owns GPU resources
	IndexBuffer(const IndexBuffer&) = delete;
	IndexBuffer& operator=(const IndexBuffer&) = delete;

	// Movable
	IndexBuffer(IndexBuffer&&) noexcept = default;
	IndexBuffer& operator=(IndexBuffer&&) noexcept = default;

	/** @brief Number of indices in the buffer. */
	uint32_t GetIndexCount() const { return b_indexCount; }

	/** @brief Index type - always VK_INDEX_TYPE_UINT32. */
	vk::IndexType GetIndexType() const { return vk::IndexType::eUint32; }

private:
	uint32_t b_indexCount;
};

} // namespace neurus
