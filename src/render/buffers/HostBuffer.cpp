#include "HostBuffer.h"

#include <cstring>
#include <stdexcept>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

HostBuffer::HostBuffer(const vk::raii::Device& device,
                       const vk::raii::PhysicalDevice& physicalDevice,
                       vk::DeviceSize size,
                       vk::BufferUsageFlags usage,
                       const char* debugName)
{
	if (size == 0)
	{
		throw std::runtime_error("HostBuffer: capacity must be non-zero (it cannot grow later).");
	}

	createBuffer(device, physicalDevice,
	             size,
	             usage,
	             vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
	             debugName);

	// Mapped once and kept mapped: repeated map/unmap around every write costs a
	// driver round-trip for no benefit, and Vulkan places no limit on how long a
	// host-visible allocation may stay mapped.
	h_mapped = b_memory->mapMemory(0, b_size);
	b_state = BufferState::HostWrite;
}

HostBuffer::~HostBuffer()
{
	// b_memory is a raii handle that unmaps nothing on its own, so the mapping
	// this class opened has to be closed here — before the handle is destroyed by
	// the base class.
	if (h_mapped && b_memory)
	{
		b_memory->unmapMemory();
		h_mapped = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Write
// ---------------------------------------------------------------------------

void HostBuffer::Write(const void* data, vk::DeviceSize size)
{
	if (size > b_size)
	{
		throw std::runtime_error("HostBuffer::Write: data size exceeds fixed capacity.");
	}

	if (size == 0)
	{
		return;
	}

	std::memcpy(h_mapped, data, static_cast<size_t>(size));
	b_state = BufferState::HostWrite;
}

} // namespace neurus
