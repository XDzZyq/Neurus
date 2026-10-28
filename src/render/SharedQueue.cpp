/**
 * @file SharedQueue.cpp
 * @brief Implementation of SharedQueue.
 */

#include "SharedQueue.h"

#include "core/Log.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace neurus
{

SharedQueue::SharedQueue(const vk::raii::Device& device, vk::Queue queue, uint32_t familyIndex)
	: m_device(device)
	, m_queue(queue)
	, m_familyIndex(familyIndex)
{
	NEURUS_LOG("[SharedQueue] Wrapping queue from family " << m_familyIndex);
}

void SharedQueue::Submit(const vk::SubmitInfo& info, vk::Fence fence)
{
	// vkQueueSubmit itself is the critical section: the GPU work it hands over
	// outlives the call, and waiting for it here would stall every other
	// submitter for as long as this submission runs.
	std::lock_guard<std::mutex> lock(m_mutex);
	m_queue.submit(info, fence);
}

void SharedQueue::SubmitAndWait(const vk::SubmitInfo& info)
{
	// Fence creation belongs to the device, not the queue, so it needs no
	// queue-level synchronisation and stays outside the lock.
	vk::raii::Fence fence(m_device, vk::FenceCreateInfo{});

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_queue.submit(info, *fence);
	}

	// The lock is released before the wait: the fence already isolates this
	// caller from every other submission on the queue, while holding the lock
	// here would block a concurrent submit or present for the whole duration of
	// this work - a long bake would freeze the frame loop.
	const vk::Result result = m_device.waitForFences(*fence, VK_TRUE, UINT64_MAX);
	if (result != vk::Result::eSuccess)
	{
		throw std::runtime_error("SharedQueue::SubmitAndWait: waitForFences returned "
		                         + vk::to_string(result));
	}
}

vk::Result SharedQueue::Present(const vk::PresentInfoKHR& info)
{
	// vkQueuePresentKHR also requires external synchronisation. The result and
	// any exception are passed through untouched: deciding what an out-of-date
	// swapchain means belongs to the swapchain owner.
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_queue.presentKHR(info);
}

void SharedQueue::WaitIdle()
{
	// The one place the lock stays held across a GPU wait. Callers wait only for
	// a swapchain rebuild, a surface change, shutdown or a failed frame-fence
	// drain, rare enough that blocking other submitters is accepted - and letting
	// a submit through after the drain started would leave exactly the in-flight
	// work this call promises is gone.
	std::lock_guard<std::mutex> lock(m_mutex);
	m_queue.waitIdle();
}

} // namespace neurus
