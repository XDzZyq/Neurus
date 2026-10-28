#pragma once

#include "Buffer.h"

#include <vulkan/vulkan_raii.hpp>

#include <memory>

namespace neurus {

class SharedQueue;

/**
 * @brief Host-visible staging buffer for CPU↔GPU data transfer.
 *
 * Allocated with HOST_VISIBLE | HOST_COHERENT memory so the CPU can write
 * directly without explicit flush/invalidate calls. Owns a transient command
 * pool and one-shot command buffer: call BeginStaging() to start recording
 * transfer commands, then EndStaging() to submit and wait.
 */
class StagingBuffer : public Buffer
{
public:
	/**
	 * @brief Creates a host-visible staging buffer.
	 *
	 * Created with @p extraUsage OR-ed with eTransferSrc; pass eTransferDst
	 * for readback. Memory is HOST_VISIBLE | HOST_COHERENT.
	 *
	 * @param device           Borrowed logical device (outlives this buffer).
	 * @param physicalDevice   Borrowed physical device for memory queries.
	 * @param size             Buffer size in bytes.
	 * @param debugName        Optional debug name.
	 * @param extraUsage       Additional buffer usage flags (OR-ed with eTransferSrc).
	 */
	StagingBuffer(const vk::raii::Device& device,
	              const vk::raii::PhysicalDevice& physicalDevice,
	              vk::DeviceSize size,
	              const char* debugName = nullptr,
	              vk::BufferUsageFlags extraUsage = {});

	// Inherits non-copyable / movable from Buffer — no need to re-delete

	/**
	 * @brief Uploads host data directly into host-visible memory.
	 *
	 * A plain memcpy - no GPU copy, no submit.
	 *
	 * @param data Pointer to source data on the host.
	 * @param size Number of bytes to copy (must not exceed buffer size).
	 */
	void Upload(const void* data, vk::DeviceSize size) override;

	/**
	 * @brief Maps the host-visible memory for CPU read/write access.
	 * @return Writable pointer to the buffer memory.
	 */
	void* Map() override;

	/**
	 * @brief Unmaps the host-visible memory.
	 */
	void Unmap() override;

	// --- Transient command buffer lifecycle ---

	/**
	 * @brief Begins (or re-begins) the transient one-shot command buffer.
	 *
	 * Lazily creates the command pool on first call; later calls reset the
	 * pool and re-begin.
	 *
	 * @param queueFamilyIndex Queue family index for the transient command pool.
	 * @return Reference to the begun one-shot command buffer.
	 */
	vk::raii::CommandBuffer& BeginStaging(uint32_t queueFamilyIndex);

	/**
	 * @brief Ends the command buffer, submits it to @p queue, and waits for
	 *        that submission to complete.
	 *
	 * Must be paired with a prior BeginStaging() call. The wait covers only this
	 * submit, not the whole queue, so other submitters are not blocked behind it.
	 *
	 * @param queue Queue to submit the transfer command buffer to.
	 */
	void EndStaging(SharedQueue& queue);

private:
	std::unique_ptr<vk::raii::CommandPool> b_cmdPool;
	std::unique_ptr<vk::raii::CommandBuffers> b_cmdBufs;
};

} // namespace neurus
