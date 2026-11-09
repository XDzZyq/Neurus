/**
 * @file TimelineSemaphore.cpp
 * @brief Implementation of TimelineSemaphore.
 */

#include "TimelineSemaphore.h"

#include "core/Log.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace neurus
{

TimelineSemaphore::TimelineSemaphore(const vk::raii::Device& device, const char* debugName)
	: m_device(device)
{
	// The type and the starting value live in the pNext chain: a timeline
	// semaphore is created *as* a counter, so the type cannot be changed later.
	const vk::SemaphoreTypeCreateInfo typeInfo(vk::SemaphoreType::eTimeline, 0);
	const vk::SemaphoreCreateInfo createInfo({}, &typeInfo);

	m_semaphore = vk::raii::Semaphore(device, createInfo);

#ifdef _DEBUG
	if (debugName && *debugName)
	{
		const vk::DebugUtilsObjectNameInfoEXT nameInfo(
			vk::ObjectType::eSemaphore,
			reinterpret_cast<uint64_t>(static_cast<VkSemaphore>(*m_semaphore)),
			debugName);
		device.setDebugUtilsObjectNameEXT(nameInfo);
	}
#endif

	NEURUS_LOG("[TimelineSemaphore] Created '" << (debugName ? debugName : "")
	           << "' at value 0");
}

uint64_t TimelineSemaphore::Value() const
{
	// A query, not a wait: it costs a driver call and never blocks, which is
	// what makes it usable from the main thread while the render thread draws.
	return m_semaphore.getCounterValue();
}

void TimelineSemaphore::Signal(uint64_t value)
{
	m_device.signalSemaphore(vk::SemaphoreSignalInfo(*m_semaphore, value));
}

bool TimelineSemaphore::Wait(uint64_t value, uint64_t timeoutNs) const
{
	const vk::Semaphore semaphore = *m_semaphore;
	const vk::SemaphoreWaitInfo waitInfo({}, 1, &semaphore, &value);

	// eTimeout is a success code for vkWaitSemaphores, so it arrives as a
	// result rather than an exception - exactly the "still running" answer a
	// caller polls with. Anything else it can report is a real failure.
	const vk::Result result = m_device.waitSemaphores(waitInfo, timeoutNs);
	if (result == vk::Result::eTimeout)
	{
		return false;
	}
	if (result != vk::Result::eSuccess)
	{
		throw std::runtime_error("TimelineSemaphore::Wait: waitSemaphores returned "
		                         + vk::to_string(result));
	}
	return true;
}

} // namespace neurus
