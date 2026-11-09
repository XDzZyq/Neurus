/**
 * @file SharedQueue.h
 * @brief Mutex-guarded access to one VkQueue handle.
 *
 * Vulkan requires *external* synchronisation for `vkQueueSubmit`,
 * `vkQueuePresentKHR` and `vkQueueWaitIdle`: two threads calling any of them on
 * the same queue at the same time is a validation error, not merely slow. Once
 * a second thread also owns GPU work - a background bake, an upload, an
 * offscreen render - every queue call in the process has to agree on one
 * interlock, so this type owns that interlock rather than leaving each caller to
 * invent its own.
 *
 * One instance exists per distinct VkQueue handle, and every caller submits
 * through it. The raw handle stays reachable through Handle() for the places
 * that need it without submitting (queue family selection, device creation,
 * debug naming).
 *
 * **The lock covers the call, not the GPU work.** `vkQueueSubmit` returns as
 * soon as the work is queued, so holding the mutex for the call costs
 * microseconds. Holding it until the work finished would be a different thing
 * entirely: a dedicated thread doing a multi-second bake would block the frame
 * thread's own submit and present behind it, and the UI would stall on a
 * mutex it never needed to contend for. That is why SubmitAndWait() releases
 * the lock before waiting on its fence, and why WaitIdle() - which does hold
 * the lock across the wait - is reserved for the rare events that genuinely
 * want the whole queue quiet.
 *
 * Non-copyable and non-movable: the mutex must keep one identity for as long as
 * threads can reach it, and a moved-from wrapper would leave a queue handle
 * shared by two objects with two different mutexes - exactly the bug this type
 * exists to prevent.
 *
 * @note The device reference is borrowed and must outlive the wrapper, which is
 *       naturally true when both are owned by the renderer.
 */

#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <mutex>

namespace neurus
{

/**
 * @brief Serialises every operation on one queue handle.
 */
class SharedQueue
{
public:
	/**
	 * @brief Wraps @p queue, which must belong to @p device and @p familyIndex.
	 * @param device       Borrowed logical device; also used to create the fence
	 *                     that SubmitAndWait() waits on.
	 * @param queue        The queue handle shared by every submitter.
	 * @param familyIndex  Queue family @p queue was retrieved from.
	 */
	SharedQueue(const vk::raii::Device& device, vk::Queue queue, uint32_t familyIndex);

	SharedQueue(const SharedQueue&)            = delete;
	SharedQueue& operator=(const SharedQueue&) = delete;
	SharedQueue(SharedQueue&&)                 = delete;
	SharedQueue& operator=(SharedQueue&&)      = delete;

	/**
	 * @brief Queues @p info and returns, without waiting for the GPU.
	 * @param info   Submission description; semaphores stay the caller's.
	 * @param fence  Optional fence signalled when the submitted work completes.
	 *
	 * The lock is held for the `vkQueueSubmit` call only. Whatever the caller
	 * submitted may still be running when this returns - waiting is what the
	 * caller's own fence is for.
	 */
	void Submit(const vk::SubmitInfo& info, vk::Fence fence = nullptr);

	/**
	 * @brief Queues @p info and blocks until that submission has completed.
	 * @param info  Submission description; semaphores stay the caller's.
	 *
	 * Creates its own fence, submits under the lock, then releases the lock
	 * before waiting. Another thread may therefore submit or present while this
	 * call is blocked - their work simply queues behind this one, which is the
	 * point: only the caller that owns the wait pays for it.
	 *
	 * @throws std::runtime_error when the wait does not return eSuccess (a
	 *         timeout or device loss); the message names the result code.
	 */
	void SubmitAndWait(const vk::SubmitInfo& info);

	/**
	 * @brief Presents under the lock and returns what `presentKHR` gives.
	 * @param info  Present description; swapchain and semaphores stay the
	 *              caller's.
	 * @return eSuccess or eSuboptimalKHR - the codes `presentKHR` returns
	 *         without throwing.
	 *
	 * Exceptions propagate unchanged: with the default Vulkan-HPP configuration
	 * an out-of-date swapchain arrives as `vk::OutOfDateKHRError`, and rebuilding
	 * it stays the swapchain owner's decision, not this wrapper's.
	 */
	vk::Result Present(const vk::PresentInfoKHR& info);

	/**
	 * @brief Waits for the queue to drain, holding the lock across the wait.
	 * @throws Whatever `vk::Queue::waitIdle()` throws on failure.
	 *
	 * Only for swapchain rebuild, surface change, shutdown, and the fallback when
	 * a frame-fence drain fails (a hung GPU) - rare events where blocking every
	 * other submitter is accepted, and where a submit slipping in after the wait
	 * started would defeat the drain entirely. Never call this on a per-frame
	 * path: that is the stall this type exists to avoid.
	 */
	void WaitIdle();

	/** @brief The wrapped handle, for callers that need it without submitting. */
	vk::Queue Handle() const { return m_queue; }

	/** @brief The queue family @ref Handle was retrieved from. */
	uint32_t FamilyIndex() const { return m_familyIndex; }

private:
	/// @brief Borrowed for fence creation; must outlive this wrapper.
	const vk::raii::Device& m_device;
	vk::Queue  m_queue;
	uint32_t   m_familyIndex;

	/**
	 * @brief Guards every call into the queue.
	 *
	 * Held across the call only, except in WaitIdle() - see the file comment for
	 * why a long GPU wait must not be inside it.
	 */
	std::mutex m_mutex;
};

} // namespace neurus
