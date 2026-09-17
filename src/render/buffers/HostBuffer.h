#pragma once

#include "Buffer.h"

#include <vulkan/vulkan_raii.hpp>

namespace neurus {

/**
 * @brief Host-visible buffer that shaders read directly, with no staging copy.
 *
 * Fills the gap between the other two buffer strategies:
 *   - GPUBuffer is device-local, so every write costs a staging copy plus a
 *     queue submit and waitIdle — correct for geometry uploaded once, far too
 *     expensive for data that changes as the user drags a gizmo. ArrayBuffer<T>
 *     wraps that same GPUBuffer, so it inherits the stall: it is the right
 *     vehicle for the light SSBOs, which change when a light is edited, but a
 *     gizmo overlay changes on every frame of a drag, and issue #22 asks for
 *     10,000 segments in under 0.5 ms — less than one waitIdle costs.
 *   - StagingBuffer is a transfer staging area by contract: it exists to be the
 *     source or destination of a copy and owns a transient command pool for
 *     exactly that. It is not a permanent buffer object and must not be used as
 *     one.
 *
 * HostBuffer is the permanent form: HOST_VISIBLE | HOST_COHERENT memory mapped
 * once for its whole lifetime, created with whatever shader-readable usage the
 * caller needs (eStorageBuffer, eUniformBuffer, ...). Write() is a plain memcpy
 * into memory the GPU can already see — no command buffer, no submit, no
 * barrier, since host writes to coherent memory are made visible by queue
 * submission (VkMemoryBarrier is unnecessary for HOST_COHERENT host writes
 * ordered before a submit).
 *
 * Usage:
 *   HostBuffer segs(device, pd, bytes, vk::BufferUsageFlagBits::eStorageBuffer,
 *                   "Debug Segments");
 *   set.WriteBuffer(1, segs.GetDescriptorInfo(), eStorageBuffer);  // once
 *   segs.Write(list.data(), n * sizeof(Segment));                  // on change
 *
 * @note Capacity is FIXED at construction and the class deliberately offers no
 *       growth. Reallocating would swap the VkBuffer handle and silently
 *       invalidate every descriptor already pointing at it; instead the producer
 *       clamps its item count against Capacity() and reports the overflow. This
 *       is the same trade the line batchers in Blender and Unreal make.
 * @note Not synchronized. A buffer the GPU may still be reading needs one
 *       instance per frame in flight, written only after that frame's fence has
 *       been waited on.
 * @note Neither copyable nor movable: the destructor owns the unmap, so moving
 *       would need a hand-written pair only to leave the mapping dangling in the
 *       source. Hold these by unique_ptr when a container is needed.
 */
class HostBuffer : public Buffer
{
public:
	/**
	 * @brief Creates and permanently maps a host-visible, shader-readable buffer.
	 *
	 * @param device          Borrowed logical device (must outlive this buffer).
	 * @param physicalDevice  Borrowed physical device for memory type queries.
	 * @param size            Capacity in bytes; fixed for the buffer's lifetime.
	 * @param usage           Shader usage flags (eStorageBuffer, eUniformBuffer, ...).
	 * @param debugName       Optional debug name.
	 *
	 * @throws std::runtime_error if @p size is 0, or on allocation failure.
	 */
	HostBuffer(const vk::raii::Device& device,
	           const vk::raii::PhysicalDevice& physicalDevice,
	           vk::DeviceSize size,
	           vk::BufferUsageFlags usage,
	           const char* debugName = nullptr);

	~HostBuffer() override;

	HostBuffer(const HostBuffer&) = delete;
	HostBuffer& operator=(const HostBuffer&) = delete;

	/**
	 * @brief Copies @p size bytes into the buffer.
	 *
	 * @param data Source bytes on the host; ignored when @p size is 0.
	 * @param size Byte count; must not exceed Capacity().
	 * @throws std::runtime_error if @p size exceeds Capacity().
	 */
	void Write(const void* data, vk::DeviceSize size);

	/** @brief Capacity in bytes (never changes). */
	vk::DeviceSize Capacity() const { return b_size; }

	/**
	 * @brief The permanent mapping, for callers that build data in place.
	 * @return Writable pointer to the first byte; never null after construction.
	 */
	void* Data() const { return h_mapped; }

	// --- Buffer interface ---

	/** @brief Alias for Write(); present to satisfy the Buffer interface. */
	void Upload(const void* data, vk::DeviceSize size) override { Write(data, size); }

	/** @brief Returns the permanent mapping; does not create a new one. */
	void* Map() override { return h_mapped; }

	/** @brief No-op: the mapping is held for the buffer's lifetime. */
	void Unmap() override {}

private:
	void* h_mapped = nullptr;
};

} // namespace neurus
