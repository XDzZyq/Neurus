#include "VertexBuffer.h"

#include "core/Log.h"

#include <stdexcept>

namespace neurus {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

VertexBuffer::VertexBuffer(const vk::raii::Device& device,
                           const vk::raii::PhysicalDevice& physicalDevice,
                           SharedQueue& queue,
                           uint32_t queueFamilyIndex,
                           const void* data,
                           vk::DeviceSize size,
                           uint32_t stride,
                           uint32_t vertexCount,
                           const char* debugName)
	: GPUBuffer(device,
	            physicalDevice,
	            queue,
	            queueFamilyIndex,
	            size,
	            vk::BufferUsageFlagBits::eVertexBuffer,
	            debugName)
	, b_vertexCount(vertexCount)
	, b_stride(stride)
{
	if (data == nullptr && size > 0)
	{
		throw std::runtime_error("VertexBuffer: data pointer is null but size > 0.");
	}

	if (data != nullptr && size > 0)
	{
		Upload(data, size);
	}

	NEURUS_LOG("[VertexBuffer] " << vertexCount << " verts, stride=" << stride
	          << ", size=" << size << " bytes"
	          << (debugName ? " name='" : "")
	          << (debugName ? debugName : "")
	          << (debugName ? "'" : ""));
}

} // namespace neurus
