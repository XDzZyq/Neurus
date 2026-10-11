#pragma once

#include "Buffer.h"

#include <vulkan/vulkan_raii.hpp>

#include <cstring>
#include <stdexcept>

namespace neurus {

/**
 * @brief Host-visible uniform buffer for a single struct T.
 *
 * Allocated with HOST_VISIBLE | HOST_COHERENT memory so the CPU can update
 * uniform data directly without staging. The buffer size is sizeof(T).
 *
 * @tparam T Uniform data struct type (must be trivially copyable).
 */
template<typename T>
class UniformBuffer : public Buffer
{
public:
	/**
	 * @brief Creates a uniform buffer of size sizeof(T).
	 *
	 * @param device          Borrowed logical device (outlives this buffer).
	 * @param physicalDevice  Borrowed physical device for memory queries.
	 * @param debugName       Optional debug name.
	 */
	UniformBuffer(const vk::raii::Device& device,
	              const vk::raii::PhysicalDevice& physicalDevice,
	              const char* debugName = nullptr)
	{
		createBuffer(device, physicalDevice,
		             sizeof(T),
		             vk::BufferUsageFlagBits::eUniformBuffer,
		             vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
		             debugName);
	}

	// Inherits non-copyable / movable from Buffer

	/**
	 * @brief Uploads raw byte data into the uniform buffer.
	 *
	 * @param data Pointer to source data on the host.
	 * @param size Byte count (must not exceed sizeof(T)).
	 */
	void Upload(const void* data, vk::DeviceSize size) override
	{
		if (size > sizeof(T))
		{
			throw std::runtime_error("UniformBuffer::Upload: data size exceeds buffer capacity.");
		}

		void* mapped = Map();
		std::memcpy(mapped, data, static_cast<size_t>(size));
		Unmap();
	}

	/**
	 * @brief Type-safe convenience overload for uploading a T instance.
	 *
	 * @param data Reference to the uniform data to upload.
	 */
	void Upload(const T& data)
	{
		Upload(&data, sizeof(T));
	}

	/**
	 * @brief Maps the host-visible memory for CPU write access.
	 * @return Writable pointer to the uniform buffer memory.
	 */
	void* Map() override
	{
		return b_memory->mapMemory(0, sizeof(T));
	}

	/**
	 * @brief Unmaps the host-visible memory.
	 */
	void Unmap() override
	{
		b_memory->unmapMemory();
	}
};

} // namespace neurus
