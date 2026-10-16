/**
 * @file FrameMailbox.cpp
 * @brief Implementation of FrameMailbox.
 */

#include "FrameMailbox.h"

#include <utility>

namespace neurus
{

void FrameMailbox::Publish(RenderFrameSnapshot&& snapshot)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		// Assignment into the optional overwrites an unconsumed frame - that
		// drop is the point: the producer must not wait for the renderer.
		m_slot = std::move(snapshot);
	}
	m_cv.notify_one();
}

std::optional<RenderFrameSnapshot> FrameMailbox::Acquire()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	m_cv.wait(lock, [this] { return m_stop || m_slot.has_value(); });

	// Stop is checked before the slot, so a stop that arrives alongside a frame
	// ends the render loop instead of drawing one more frame on the way out.
	if (m_stop)
		return std::nullopt;

	std::optional<RenderFrameSnapshot> frame = std::move(m_slot);
	m_slot.reset();  // Moved-from optional still holds a value; empty the slot.
	return frame;
}

void FrameMailbox::RequestStop()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stop = true;
	}
	m_cv.notify_all();
}

bool FrameMailbox::StopRequested() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_stop;
}

} // namespace neurus
