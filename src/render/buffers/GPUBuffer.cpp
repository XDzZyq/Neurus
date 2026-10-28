#include "GPUBuffer.h"

#include "render/Barrier.h"
#include "render/SharedQueue.h"
#include "core/Log.h"

#include <stdexcept>
#include <cstring>

namespace neurus {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

GPUBuffer::GPUBuffer(const vk::raii::Device& device,
                     const vk::raii::PhysicalDevice& physicalDevice,
                     SharedQueue& queue,
                     uint32_t queueFamilyIndex,
                     vk::DeviceSize size,
                     vk::BufferUsageFlags usageFlags,
                     const char* debugName)
	: b_queue(&queue)
	, b_queueFamilyIndex(queueFamilyIndex)
{
	createBuffer(device, physicalDevice,
	             size,
	             usageFlags | vk::BufferUsageFlagBits::eTransferDst,
	             vk::MemoryPropertyFlagBits::eDeviceLocal,
	             debugName);
	b_staging = std::make_unique<StagingBuffer>(*b_device,
		                                        *b_physicalDevice,
		                                        b_size,
		                                        b_debugName.empty() ? nullptr
		                                                            : (b_debugName + "_Staging").c_str());

	NEURUS_LOG("[GPUBuffer::Map] created staging buffer, size=" << b_size);
}

// ---------------------------------------------------------------------------
// Upload (Map + memcpy + Unmap)
// ---------------------------------------------------------------------------

void GPUBuffer::Upload(const void* data, vk::DeviceSize size)
{
	if (size > b_size)
	{
		throw std::runtime_error("GPUBuffer::Upload: data size exceeds buffer capacity.");
	}

	void* mapped = Map();
	std::memcpy(mapped, data, static_cast<size_t>(size));
	Unmap();
}

// ---------------------------------------------------------------------------
// Map — lazily creates staging buffer, returns writable pointer
// ---------------------------------------------------------------------------

void* GPUBuffer::Map()
{
	return b_staging->Map();
}

// ---------------------------------------------------------------------------
// Unmap — copies staging buffer to device-local via vkCmdCopyBuffer
// ---------------------------------------------------------------------------

void GPUBuffer::Unmap()
{
	b_staging->Unmap();

	auto& cmd = b_staging->BeginStaging(b_queueFamilyIndex);

	// --- Transition device-local buffer: ready to receive transfer write ---
	Barrier::Transition(cmd, *this, BufferState::TransferDst);

	vk::BufferCopy copyRegion(0, 0, b_size);
	cmd.copyBuffer(b_staging->buffer(), this->buffer(), copyRegion);

	// --- Release barrier: make transfer writes available to subsequent
	//     graphics queue usage. The destination must not be ALL_COMMANDS (invalid
	//     for transfer-only command pools per VUID-vkCmdPipelineBarrier2-dstStageMask-09676).
	{
		vk::BufferMemoryBarrier2 barrier(
			vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eTransferWrite,
			vk::PipelineStageFlagBits2::eBottomOfPipe,
			vk::AccessFlagBits2::eNone,
			0, 0,
			this->buffer(), 0, b_size);
		vk::DependencyInfo depInfo;
		depInfo.setBufferMemoryBarriers(barrier);
		cmd.pipelineBarrier2(depInfo);
	}
	// The buffer stays in TransferDst logically: the graphics queue transitions
	// it to ShaderRead/ShaderWrite on first use.
	b_state = BufferState::TransferDst;

	b_staging->EndStaging(*b_queue);
}

} // namespace neurus
