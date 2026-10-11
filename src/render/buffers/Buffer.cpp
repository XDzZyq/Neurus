#include "Buffer.h"

#include "core/Log.h"

#include <stdexcept>
#include <cstring>

namespace neurus {

// ---------------------------------------------------------------------------
// Internal helpers (anonymous namespace)
// ---------------------------------------------------------------------------

/**
 * @brief Finds the first memory type index matching the given requirements.
 */
static uint32_t findMemoryType(const vk::raii::PhysicalDevice& physicalDevice,
                               uint32_t memoryTypeBits,
                               vk::MemoryPropertyFlags requiredFlags)
{
	auto memProps = physicalDevice.getMemoryProperties();

	for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
	{
		if ((memoryTypeBits & (1u << i)) == 0)
	{
			continue;
		}

		if ((memProps.memoryTypes[i].propertyFlags & requiredFlags) == requiredFlags)
	{
			return i;
		}
	}

	throw std::runtime_error(
		"Buffer: No suitable memory type found for the requested properties.");
}

// ---------------------------------------------------------------------------
// createBuffer — shared construction logic for derived classes
// ---------------------------------------------------------------------------

void Buffer::createBuffer(const vk::raii::Device& device,
                          const vk::raii::PhysicalDevice& physicalDevice,
                          vk::DeviceSize size,
                          vk::BufferUsageFlags usageFlags,
                          vk::MemoryPropertyFlags memoryProperties,
                          const char* debugName)
{
	b_device = &device;
	b_physicalDevice = &physicalDevice;
	b_size = size;
	b_usageFlags = usageFlags;
	b_memoryProperties = memoryProperties;
	b_debugName = debugName ? debugName : "";

	// --- Create buffer ---
	vk::BufferCreateInfo bufferCI({}, b_size, b_usageFlags);
	b_buffer = std::make_unique<vk::raii::Buffer>(*b_device, bufferCI);

	// --- Query memory requirements ---
	auto memReqs = b_buffer->getMemoryRequirements();

	// --- Find memory type ---
	uint32_t memTypeIndex = findMemoryType(*b_physicalDevice,
	                                       memReqs.memoryTypeBits,
	                                       b_memoryProperties);

	// --- Allocate device memory ---
	vk::MemoryAllocateInfo allocInfo(memReqs.size, memTypeIndex);
	b_memory = std::make_unique<vk::raii::DeviceMemory>(*b_device, allocInfo);

	// --- Bind memory to buffer ---
	b_buffer->bindMemory(**b_memory, 0);

	// --- Set debug names (Debug builds only) ---
#ifdef _DEBUG
	if (!b_debugName.empty())
	{
		{
			vk::DebugUtilsObjectNameInfoEXT nameInfo(
				vk::ObjectType::eBuffer,
				reinterpret_cast<uint64_t>(static_cast<VkBuffer>(**b_buffer)),
				b_debugName.c_str());
			b_device->setDebugUtilsObjectNameEXT(nameInfo);
		}

		{
			std::string memName = b_debugName + "_Mem";
			vk::DebugUtilsObjectNameInfoEXT nameInfo(
				vk::ObjectType::eDeviceMemory,
				reinterpret_cast<uint64_t>(static_cast<VkDeviceMemory>(**b_memory)),
				memName.c_str());
			b_device->setDebugUtilsObjectNameEXT(nameInfo);
		}
	}
#endif

	NEURUS_LOG("[Buffer] size=" << b_size
	          << " usage=" << vk::to_string(b_usageFlags)
	          << " memProps=" << vk::to_string(b_memoryProperties)
	          << (!b_debugName.empty() ? " name='" : "")
	          << (!b_debugName.empty() ? b_debugName : "")
	          << (!b_debugName.empty() ? "'" : ""));
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

Buffer::Buffer(Buffer&& other) noexcept
	: b_device(other.b_device)
	, b_physicalDevice(other.b_physicalDevice)
	, b_buffer(std::move(other.b_buffer))
	, b_memory(std::move(other.b_memory))
	, b_size(other.b_size)
	, b_usageFlags(other.b_usageFlags)
	, b_memoryProperties(other.b_memoryProperties)
	, b_debugName(std::move(other.b_debugName))
{
	// Invalidate the moved-from object
	other.b_size = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
	if (this != &other)
	{
		b_device = other.b_device;
		b_physicalDevice = other.b_physicalDevice;
		b_buffer = std::move(other.b_buffer);
		b_memory = std::move(other.b_memory);
		b_size = other.b_size;
		b_usageFlags = other.b_usageFlags;
		b_memoryProperties = other.b_memoryProperties;
		b_debugName = std::move(other.b_debugName);

		// Invalidate the moved-from object
		other.b_size = 0;
	}

	return *this;
}

// ---------------------------------------------------------------------------
// GetDescriptorInfo
// ---------------------------------------------------------------------------

vk::DescriptorBufferInfo Buffer::GetDescriptorInfo(vk::DeviceSize offset,
                                                    vk::DeviceSize range) const
{
	vk::DeviceSize actualRange = (range == VK_WHOLE_SIZE) ? b_size : range;
	return vk::DescriptorBufferInfo(this->buffer(), offset, actualRange);
}

} // namespace neurus
