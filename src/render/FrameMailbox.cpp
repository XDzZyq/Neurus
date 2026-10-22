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

AcquireStatus FrameMailbox::Acquire(RenderFrameSnapshot& out)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	m_cv.wait(lock, [this] { return m_stop || m_wake || m_slot.has_value(); });

	// Priority order: stop, then wake, then frame. Stop ends the loop even with
	// a frame or wake pending. A pending wake outranks a frame so a control
	// message posted just before the frame is serviced before it is drawn - the
	// frame stays in the slot for the next Acquire().
	if (m_stop)
		return AcquireStatus::Stopped;

	if (m_wake)
	{
		// Consume the wake: coalesced Wake() calls become a single Interrupted,
		// since the consumer drains its whole control queue on each return.
		m_wake = false;
		return AcquireStatus::Interrupted;
	}

	out = std::move(*m_slot);
	m_slot.reset();  // Moved-from optional still holds a value; empty the slot.
	return AcquireStatus::Frame;
}

void FrameMailbox::Wake()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		// Raised under the mutex the consumer waits on, so a Wake() racing a
		// consumer about to park cannot slip past the wait predicate.
		m_wake = true;
	}
	m_cv.notify_one();
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
