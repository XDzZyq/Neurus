#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>

namespace neurus {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/** @brief Default fence timeout: 100 ms - finite timeout prevents main-thread deadlock. */
constexpr uint64_t kDefaultFenceTimeoutNs = 100'000'000;

// ---------------------------------------------------------------------------
// Fence
// ---------------------------------------------------------------------------

/**
 * @brief RAII wrapper around a single vk::raii::Fence.
 *
 * WaitAndReset() waits for the GPU to signal the fence, then resets it to
 * the unsignaled state. Non-copyable; move-only.
 */
class Fence
{
public:
	/**
	 * @brief Creates a fence.
	 * @param device  Logical device (must outlive this fence).
	 * @param flags   Fence creation flags (default: 0 = unsignaled).
	 */
	explicit Fence(const vk::raii::Device& device, vk::FenceCreateFlags flags = {});

	~Fence() = default;

	Fence(const Fence&) = delete;
	Fence& operator=(const Fence&) = delete;
	Fence(Fence&&) noexcept = default;
	Fence& operator=(Fence&&) noexcept = default;

	/**
	 * @brief Waits for the fence to be signaled, then resets it to unsignaled.
	 *
	 * If the fence is already signaled (e.g. from a previous submit or
	 * VK_FENCE_CREATE_SIGNALED_BIT), returns immediately.
	 *
	 * @param timeoutNs  Timeout in nanoseconds (default: kDefaultFenceTimeoutNs).
	 * @return true  if the fence was signaled before the timeout expired.
	 * @return false if the wait timed out (fence was not signaled).
	 */
	bool WaitAndReset(uint64_t timeoutNs = kDefaultFenceTimeoutNs);

	/** @brief Returns a const reference to the underlying vk::raii::Fence. */
	const vk::raii::Fence& handle() const { return sync_fence; }

private:
	const vk::raii::Device* sync_device;
	vk::raii::Fence sync_fence;
};

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

/**
 * @brief RAII wrapper around a single vk::raii::Semaphore.
 *
 * Non-copyable; move-only.
 */
class Semaphore
{
public:
	/**
	 * @brief Creates a binary semaphore.
	 * @param device  Logical device (must outlive this semaphore).
	 */
	explicit Semaphore(const vk::raii::Device& device);

	~Semaphore() = default;

	Semaphore(const Semaphore&) = delete;
	Semaphore& operator=(const Semaphore&) = delete;
	Semaphore(Semaphore&&) noexcept = default;
	Semaphore& operator=(Semaphore&&) noexcept = default;

	/** @brief Returns a const reference to the underlying vk::raii::Semaphore. */
	const vk::raii::Semaphore& handle() const { return sync_semaphore; }

private:
	const vk::raii::Device* sync_device;
	vk::raii::Semaphore sync_semaphore;
};

// ---------------------------------------------------------------------------
// FrameSync
// ---------------------------------------------------------------------------

/**
 * @brief Synchronization primitives for one frame-in-flight slot.
 *
 * Aggregates the three objects needed for a single frame:
 *   - inFlight          fence, signaled when the GPU has finished this frame
 *   - imageAvailable    semaphore, signaled when the swapchain image is ready
 *   - renderFinished    semaphore, signaled when rendering is complete
 */
struct FrameSync
{
	Fence inFlight;
	Semaphore imageAvailable;
	Semaphore renderFinished;

	/**
	 * @brief Constructs all three synchronization objects.
	 * @param device      Logical device (must outlive all objects).
	 * @param fenceFlags  Fence creation flags (default: eSignaled so the first
	 *                    WaitAndReset returns immediately).
	 */
	FrameSync(const vk::raii::Device& device,
	          vk::FenceCreateFlags fenceFlags = vk::FenceCreateFlagBits::eSignaled);
};

// ---------------------------------------------------------------------------
// Barrier helpers
// ---------------------------------------------------------------------------

/**
 * @brief Creates a vk::BufferMemoryBarrier2 for pipeline barrier commands.
 *
 * Default parameters target the full buffer with no queue-family transfer.
 *
 * @param buffer               The buffer to synchronise.
 * @param offset               Byte offset into the buffer.
 * @param size                 Byte size of the range (default: VK_WHOLE_SIZE).
 * @param srcStage             Source pipeline stage mask.
 * @param srcAccess            Source access mask.
 * @param dstStage             Destination pipeline stage mask.
 * @param dstAccess            Destination access mask.
 * @param srcQueueFamilyIndex  Source queue family (default: VK_QUEUE_FAMILY_IGNORED).
 * @param dstQueueFamilyIndex  Destination queue family (default: VK_QUEUE_FAMILY_IGNORED).
 * @return A fully populated vk::BufferMemoryBarrier2.
 */
[[nodiscard]] inline vk::BufferMemoryBarrier2 BufferBarrier(
	vk::Buffer buffer,
	vk::DeviceSize offset,
	vk::DeviceSize size = VK_WHOLE_SIZE,
	vk::PipelineStageFlags2 srcStage = vk::PipelineStageFlagBits2::eTopOfPipe,
	vk::AccessFlags2 srcAccess = vk::AccessFlagBits2::eNone,
	vk::PipelineStageFlags2 dstStage = vk::PipelineStageFlagBits2::eBottomOfPipe,
	vk::AccessFlags2 dstAccess = vk::AccessFlagBits2::eNone,
	uint32_t srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	uint32_t dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED)
{
	return vk::BufferMemoryBarrier2(
		srcStage,
		srcAccess,
		dstStage,
		dstAccess,
		srcQueueFamilyIndex,
		dstQueueFamilyIndex,
		buffer,
		offset,
		size);
}

// ---------------------------------------------------------------------------
// Inline implementations
// ---------------------------------------------------------------------------

inline Fence::Fence(const vk::raii::Device& device, vk::FenceCreateFlags flags)
	: sync_device(&device)
	, sync_fence(device, vk::FenceCreateInfo(flags))
{
}

inline bool Fence::WaitAndReset(uint64_t timeoutNs)
{
	auto result = sync_device->waitForFences(*sync_fence, VK_TRUE, timeoutNs);
	if (result == vk::Result::eSuccess)
	{
		sync_device->resetFences(*sync_fence);
		return true;
	}
	return false;
}

inline Semaphore::Semaphore(const vk::raii::Device& device)
	: sync_device(&device)
	, sync_semaphore(device, vk::SemaphoreCreateInfo())
{
}

inline FrameSync::FrameSync(const vk::raii::Device& device,
                             vk::FenceCreateFlags fenceFlags)
	: inFlight(device, fenceFlags)
	, imageAvailable(device)
	, renderFinished(device)
{
}

} // namespace neurus
