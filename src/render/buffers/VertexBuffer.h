#pragma once

#include "GPUBuffer.h"

#include <vulkan/vulkan_raii.hpp>

namespace neurus {

/**
 * @brief Device-local vertex buffer with staging-backed upload.
 *
 * Inherits the buffer lifecycle (device-local allocation, staging-backed
 * Map/Unmap/Upload) from GPUBuffer and specializes the usage to
 * VERTEX_BUFFER.
 *
 * @note Template-free design - vertex data is passed as raw bytes + stride.
 *       Callers must ensure the data pointer remains valid during construction.
 */
class VertexBuffer : public GPUBuffer
{
public:
	/**
	 * @brief Creates a device-local vertex buffer and uploads vertex data.
	 *
	 * @param device           Borrowed logical device (outlives this buffer).
	 * @param physicalDevice   Borrowed physical device for memory queries.
	 * @param queue            Borrowed graphics queue for staging submits.
	 * @param queueFamilyIndex Queue family index for temp command pool creation.
	 * @param data             Pointer to host vertex data (raw bytes).
	 * @param size             Total byte size of vertex data (vertexCount * stride).
	 * @param stride           Byte stride between consecutive vertices.
	 * @param vertexCount      Number of vertices (must match size / stride).
	 * @param debugName        Optional debug name (set via VK_EXT_debug_utils in Debug builds).
	 */
	VertexBuffer(const vk::raii::Device& device,
	             const vk::raii::PhysicalDevice& physicalDevice,
	             vk::Queue queue,
	             uint32_t queueFamilyIndex,
	             const void* data,
	             vk::DeviceSize size,
	             uint32_t stride,
	             uint32_t vertexCount,
	             const char* debugName = nullptr);

	// Non-copyable - owns GPU resources
	VertexBuffer(const VertexBuffer&) = delete;
	VertexBuffer& operator=(const VertexBuffer&) = delete;

	// Movable
	VertexBuffer(VertexBuffer&&) noexcept = default;
	VertexBuffer& operator=(VertexBuffer&&) noexcept = default;

	/** @brief Number of vertices in the buffer. */
	uint32_t GetVertexCount() const { return b_vertexCount; }

	/** @brief Byte stride between consecutive vertices. */
	uint32_t GetStride() const { return b_stride; }

private:
	uint32_t b_vertexCount;
	uint32_t b_stride;
};

} // namespace neurus
