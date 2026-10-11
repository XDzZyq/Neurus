#include "CPUBuffer.h"

#include <cstring>
#include <stdexcept>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CPUBuffer::CPUBuffer(const vk::raii::Device& device,
                     const vk::raii::PhysicalDevice& physicalDevice,
                     vk::DeviceSize size,
                     vk::BufferUsageFlags usage,
                     const char* debugName)
{
	if (size == 0)
	{
		throw std::runtime_error("CPUBuffer: capacity must be non-zero (one instance cannot grow).");
	}

	createBuffer(device, physicalDevice,
	             size,
	             usage,
	             vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
	             debugName);

	// Mapped once and kept mapped: repeated map/unmap around every write costs
	// a driver round-trip for no benefit, and Vulkan allows long-lived mappings.
	h_mapped = b_memory->mapMemory(0, b_size);
	b_state = BufferState::HostWrite;
}

CPUBuffer::~CPUBuffer()
{
	// The raii memory handle does not unmap on its own, so the mapping this
	// class opened has to be closed here - before the base class destroys the
	// handle.
	if (h_mapped && b_memory)
	{
		b_memory->unmapMemory();
		h_mapped = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Write
// ---------------------------------------------------------------------------

void CPUBuffer::Write(const void* data, vk::DeviceSize size)
{
	if (size > b_size)
	{
		throw std::runtime_error("CPUBuffer::Write: data size exceeds this buffer's capacity.");
	}

	if (size == 0)
	{
		return;
	}

	std::memcpy(h_mapped, data, static_cast<size_t>(size));
	b_state = BufferState::HostWrite;
}

} // namespace neurus
