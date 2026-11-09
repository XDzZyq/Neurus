#include "StagingBuffer.h"

#include "core/Log.h"
#include "render/SharedQueue.h"

#include <stdexcept>
#include <cstring>

namespace neurus {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

StagingBuffer::StagingBuffer(const vk::raii::Device& device,
                             const vk::raii::PhysicalDevice& physicalDevice,
                             vk::DeviceSize size,
                             const char* debugName,
                             vk::BufferUsageFlags extraUsage)
{
	createBuffer(device, physicalDevice,
	             size,
	             vk::BufferUsageFlagBits::eTransferSrc | extraUsage,
	             vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
	             debugName);
}

// ---------------------------------------------------------------------------
// Upload (Map + memcpy + Unmap)
// ---------------------------------------------------------------------------

void StagingBuffer::Upload(const void* data, vk::DeviceSize size)
{
	if (size > b_size)
	{
		throw std::runtime_error("StagingBuffer::Upload: data size exceeds buffer capacity.");
	}

	void* mapped = Map();
	std::memcpy(mapped, data, static_cast<size_t>(size));
	Unmap();

	NEURUS_LOG("[StagingBuffer::Upload] " << size << " bytes written");
}

// ---------------------------------------------------------------------------
// Map / Unmap
// ---------------------------------------------------------------------------

void* StagingBuffer::Map()
{
	return b_memory->mapMemory(0, b_size);
}

void StagingBuffer::Unmap()
{
	b_memory->unmapMemory();
}

// ---------------------------------------------------------------------------
// Transient command buffer lifecycle
// ---------------------------------------------------------------------------

vk::raii::CommandBuffer& StagingBuffer::BeginStaging(uint32_t queueFamilyIndex)
{
	if (!b_cmdPool)
	{
		// --- First call: create transient pool + one command buffer ---
		const vk::CommandPoolCreateInfo poolCI(
			vk::CommandPoolCreateFlagBits::eTransient,
			queueFamilyIndex);
		b_cmdPool = std::make_unique<vk::raii::CommandPool>(*b_device, poolCI);

		const vk::CommandBufferAllocateInfo allocInfo(
			**b_cmdPool, vk::CommandBufferLevel::ePrimary, 1);
		b_cmdBufs = std::make_unique<vk::raii::CommandBuffers>(*b_device, allocInfo);
	}
	else
	{
		// --- Subsequent calls: reset pool for reuse ---
		b_cmdPool->reset();
	}

	auto& cmd = (*b_cmdBufs)[0];
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	return cmd;
}

void StagingBuffer::EndStaging(SharedQueue& queue)
{
	auto& cmd = (*b_cmdBufs)[0];
	cmd.end();

	const vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmd));
	queue.SubmitAndWait(submitInfo);
}

} // namespace neurus
