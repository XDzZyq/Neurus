#pragma once

#include "GPUBuffer.h"
#include "render/SharedQueue.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace neurus {

/**
 * @brief Templated GPU-side array buffer, analogous to std::vector on the GPU.
 *
 * Owns a device-local GPUBuffer, created lazily on the first Resize(),
 * Upload() or Update(). Resize() preserves existing content via
 * vkCmdCopyBuffer and adds eTransferSrc and eTransferDst to the usage
 * flags so every buffer can serve as a copy source and destination.
 *
 * @tparam T Element type (must be trivially copyable).
 */
template<typename T>
class ArrayBuffer
{
public:
	/**
	 * @brief Constructs an array buffer without allocating GPU memory.
	 *
	 * @param device          Borrowed logical device (must outlive this object).
	 * @param physicalDevice  Borrowed physical device for memory queries.
	 * @param queue           Borrowed graphics queue for staging/resize transfers.
	 * @param queueFamilyIndex Queue family index for transient command pools.
	 * @param usageFlags      Primary buffer usage flags.
	 *                        eTransferSrc | eTransferDst are added internally.
	 * @param debugName       Optional debug name (string literal — must outlive).
	 */
	ArrayBuffer(const vk::raii::Device& device,
	            const vk::raii::PhysicalDevice& physicalDevice,
	            SharedQueue& queue,
	            uint32_t queueFamilyIndex,
	            vk::BufferUsageFlags usageFlags,
	            const char* debugName = nullptr)
		: m_device(device)
		, m_physicalDevice(physicalDevice)
		, m_queue(&queue)
		, m_queueFamilyIndex(queueFamilyIndex)
		, m_usageFlags(usageFlags)
		, m_debugName(debugName)
	{
	}

	// Non-copyable — owns GPU resources (reference members prevent move-assign)
	ArrayBuffer(const ArrayBuffer&) = delete;
	ArrayBuffer& operator=(const ArrayBuffer&) = delete;

	// Movable (move-construction binds references at init time)
	ArrayBuffer(ArrayBuffer&&) noexcept = default;

	// --- Capacity ---

	/** @brief Number of elements currently stored in the buffer. */
	uint32_t size() const { return m_size; }

	/**
	 * @brief Number of elements the GPU buffer can hold.
	 * @return Allocated element count, or 0 if uninitialised.
	 */
	uint32_t capacity() const { return m_capacity; }

	/** @brief True when no GPU buffer has been allocated. */
	bool empty() const { return m_buffer == nullptr; }

	// --- Accessors ---

	/**
	 * @brief Returns a non-owning pointer to the underlying GPUBuffer.
	 * @return Pointer to GPUBuffer, or nullptr when uninitialised.
	 */
	const GPUBuffer* gpuBuffer() const { return m_buffer.get(); }

	/**
	 * @brief Returns the raw vk::Buffer handle.
	 * @return Valid handle, or nullptr when uninitialised.
	 */
	vk::Buffer buffer() const { return m_buffer ? m_buffer->buffer() : nullptr; }

	// --- Operations ---

	/**
	 * @brief Resizes the GPU buffer to hold exactly @a newSize elements.
	 *
	 * If a buffer already exists and @a newSize differs from the current
	 * capacity, a new GPUBuffer is allocated and the old content is copied
	 * via vkCmdCopyBuffer, preserving up to min(m_size, newSize) elements.
	 * If @a newSize equals the current capacity this is a no-op; if it is
	 * zero the buffer is released.
	 *
	 * @param newSize Desired element count.
	 */
	void Resize(uint32_t newSize)
	{
		if (newSize == m_capacity)
		{
			return;
		}

		const vk::DeviceSize newSizeBytes = static_cast<vk::DeviceSize>(newSize) * sizeof(T);

		if (newSize == 0)
		{
			m_buffer.reset();
			m_size = 0;
			m_capacity = 0;
			return;
		}

		// --- Create new GPUBuffer ---
		// eTransferSrc lets the buffer serve as the copy source in a future
		// Resize().
		auto newBuffer = std::make_unique<GPUBuffer>(
			m_device, m_physicalDevice, *m_queue, m_queueFamilyIndex,
			newSizeBytes,
			m_usageFlags | vk::BufferUsageFlagBits::eTransferSrc,
			m_debugName);

		// --- Copy old content into new buffer (if any) ---
		if (m_buffer && m_size > 0)
		{
			const uint32_t copyElements = (std::min)(m_size, newSize);
			const vk::DeviceSize copyBytes = static_cast<vk::DeviceSize>(copyElements) * sizeof(T);

			if (copyBytes > 0)
			{
				// --- Transient command pool & one-shot command buffer ---
				vk::CommandPoolCreateInfo poolCI(
					vk::CommandPoolCreateFlagBits::eTransient,
					m_queueFamilyIndex);
				vk::raii::CommandPool cmdPool(m_device, poolCI);

				vk::CommandBufferAllocateInfo allocInfo(
					*cmdPool,
					vk::CommandBufferLevel::ePrimary, 1);
				vk::raii::CommandBuffers cmdBufs(m_device, allocInfo);

				vk::CommandBufferBeginInfo beginInfo(
					vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
				cmdBufs[0].begin(beginInfo);

				vk::BufferCopy copyRegion(0, 0, copyBytes);
				cmdBufs[0].copyBuffer(
					m_buffer->buffer(),
					newBuffer->buffer(),
					copyRegion);

				// Memory barrier: make the copy visible to subsequent
				// operations on the new buffer.
				vk::MemoryBarrier barrier(
					vk::AccessFlagBits::eTransferWrite,
					vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite);
				cmdBufs[0].pipelineBarrier(
					vk::PipelineStageFlagBits::eTransfer,
					vk::PipelineStageFlagBits::eAllCommands,
					{}, {barrier}, {}, {});

				cmdBufs[0].end();

				vk::SubmitInfo submitInfo({}, {}, *cmdBufs[0]);
				m_queue->SubmitAndWait(submitInfo);
			}
		}

		// --- Swap in the new buffer ---
		m_buffer = std::move(newBuffer);
		m_capacity = newSize;
		if (newSize < m_size)
		{
			m_size = newSize;
		}
	}

	/**
	 * @brief Uploads the contents of a host-side vector to the GPU buffer.
	 *
	 * If the current capacity is smaller than @a array.size(), the buffer
	 * is automatically resized first.  After upload, size() equals
	 * array.size().
	 *
	 * @param array Host-side data to upload.
	 */
	void Upload(const std::vector<T>& array)
	{
		const uint32_t count = static_cast<uint32_t>(array.size());

		if (count > m_capacity)
		{
			Resize(count);
		}

		if (count == 0)
		{
			m_size = 0;
			return;
		}

		const vk::DeviceSize byteSize = static_cast<vk::DeviceSize>(count) * sizeof(T);

		void* mapped = m_buffer->Map();
		std::memcpy(mapped, array.data(), static_cast<size_t>(byteSize));
		m_buffer->Unmap();

		m_size = count;
	}

	/**
	 * @brief Updates a single element at the given index.
	 *
	 * The index must be in range [0, size()).
	 *
	 * @param element New element value.
	 * @param index   Zero-based element index.
	 * @throws std::out_of_range if @a index >= size() or buffer is uninitialised.
	 */
	void Update(const T& element, uint32_t index)
	{
		if (!m_buffer || index >= m_size)
		{
			throw std::out_of_range(
				"ArrayBuffer::Update: index out of range or buffer uninitialised.");
		}

		const vk::DeviceSize offset = static_cast<vk::DeviceSize>(index) * sizeof(T);

		void* mapped = m_buffer->Map();
		std::memcpy(static_cast<char*>(mapped) + offset, &element, sizeof(T));
		m_buffer->Unmap();
	}

private:
	// --- Borrowed Vulkan handles (must outlive this object) ---
	const vk::raii::Device& m_device;
	const vk::raii::PhysicalDevice& m_physicalDevice;
	SharedQueue* m_queue = nullptr;   ///< Borrowed wrapper; must outlive this buffer.
	uint32_t m_queueFamilyIndex;
	vk::BufferUsageFlags m_usageFlags;
	const char* m_debugName;

	// --- Owned GPU buffer ---
	std::unique_ptr<GPUBuffer> m_buffer;

	// --- Metadata ---
	uint32_t m_size = 0;       ///< Logical element count
	uint32_t m_capacity = 0;   ///< Allocated element count
};

} // namespace neurus
