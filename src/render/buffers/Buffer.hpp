#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <string>

namespace neurus {

/**
 * @brief Logical state of a GPU buffer for barrier tracking.
 *
 * Models the pipeline stage + access mask the buffer's contents are in.
 */
enum class BufferState
{
	Undefined,   // Initial: no valid data, any access allowed
	HostWrite,   // CPU has written data (after Upload / Map+Unmap)
	HostRead,    // CPU can read data (after transfer→host barrier)
	TransferSrc, // GPU transfer source (before vkCmdCopyBufferToImage)
	TransferDst, // GPU transfer destination (after vkCmdCopyImageToBuffer)
	ShaderRead,  // Shader can read (uniform buffer, SSBO read)
	ShaderWrite, // Shader can write (SSBO write)
	General,     // All pipeline stages can read/write (after staging→device copy)
};

/**
 * @brief Virtual base class for all GPU buffer types.
 *
 * Encapsulates common buffer lifecycle: creation, memory allocation,
 * binding, debug naming, and descriptor info. Derived classes implement
 * the data transfer strategy (Map/Unmap/Upload) appropriate for their
 * memory domain.
 *
 * @note Non-copyable, movable. The owning device must outlive this buffer.
 */
class Buffer
{
	friend class Barrier;

public:
	virtual ~Buffer() = default;

	// Non-copyable — owns GPU resources
	Buffer(const Buffer&) = delete;
	Buffer& operator=(const Buffer&) = delete;

	// Movable
	Buffer(Buffer&&) noexcept;
	Buffer& operator=(Buffer&&) noexcept;

	// --- Virtual interface: data transfer strategy ---

	/**
	 * @brief Uploads host data to the buffer.
	 * @param data Pointer to source data on the host.
	 * @param size Number of bytes to upload (must not exceed buffer size).
	 */
	virtual void Upload(const void* data, vk::DeviceSize size) = 0;

	/**
	 * @brief Maps buffer memory for direct host access.
	 *
	 * Only valid for host-visible memory.
	 *
	 * @return Mapped pointer to buffer memory.
	 */
	virtual void* Map() = 0;

	/**
	 * @brief Unmaps previously mapped buffer memory.
	 *
	 * After unmap, the mapped pointer becomes invalid.
	 */
	virtual void Unmap() = 0;

	// --- Common non-virtual accessors ---

	/**
	 * @brief Returns a VkDescriptorBufferInfo for descriptor writes.
	 *
	 * @param offset Byte offset into the buffer (default: 0).
	 * @param range  Byte range, or VK_WHOLE_SIZE for the full buffer (default).
	 * @return Descriptor buffer info referencing this buffer.
	 */
	vk::DescriptorBufferInfo GetDescriptorInfo(
		vk::DeviceSize offset = 0,
		vk::DeviceSize range = VK_WHOLE_SIZE) const;

	/** @brief Returns the underlying vk::Buffer handle. */
	vk::Buffer buffer() const { return *b_buffer; }

	/** @brief Returns the buffer size in bytes. */
	vk::DeviceSize size() const { return b_size; }

	/** @brief Returns the current logical buffer state (for barrier tracking). */
	BufferState State() const { return b_state; }

protected:
	/**
	 * @brief Default constructor for move operations (derived classes only).
	 */
	Buffer() = default;

	/**
	 * @brief Creates the underlying VkBuffer, allocates and binds device
	 *        memory, and optionally sets debug names.
	 *
	 * Called by derived-class constructors; throws std::runtime_error on failure.
	 *
	 * @param device          Borrowed logical device (must outlive this buffer).
	 * @param physicalDevice  Borrowed physical device for memory type queries.
	 * @param size            Buffer size in bytes.
	 * @param usageFlags      Vulkan buffer usage flags.
	 * @param memoryProperties Desired memory property flags.
	 * @param debugName       Optional debug name (set via VK_EXT_debug_utils in Debug).
	 */
	void createBuffer(const vk::raii::Device& device,
	                  const vk::raii::PhysicalDevice& physicalDevice,
	                  vk::DeviceSize size,
	                  vk::BufferUsageFlags usageFlags,
	                  vk::MemoryPropertyFlags memoryProperties,
	                  const char* debugName = nullptr);

	// --- Borrowed references (must outlive this object) ---
	const vk::raii::Device* b_device = nullptr;
	const vk::raii::PhysicalDevice* b_physicalDevice = nullptr;

	std::unique_ptr<vk::raii::Buffer> b_buffer;
	std::unique_ptr<vk::raii::DeviceMemory> b_memory;
	vk::DeviceSize b_size = 0;
	vk::BufferUsageFlags b_usageFlags;
	vk::MemoryPropertyFlags b_memoryProperties;
	std::string b_debugName;

	BufferState b_state = BufferState::Undefined;
};

} // namespace neurus
