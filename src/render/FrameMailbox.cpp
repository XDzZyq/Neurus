/**
 * @file FrameMailbox.cpp
 * @brief Implementation of FrameMailbox.
 */

#include "FrameMailbox.h"

#include <iterator>
#include <utility>

namespace neurus
{

void FrameMailbox::Publish(PublishedFrame&& frame)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		// The frame in the slot was never acquired, so its snapshot is redundant -
		// but the requests that rode it are not: they were posted by the Editor
		// and have to execute. They go in front of anything carried already, so
		// the mailbox stays FIFO across drops.
		if (m_slot.has_value())
		{
			m_carried.insert(m_carried.end(),
			                 std::make_move_iterator(m_slot->requests.begin()),
			                 std::make_move_iterator(m_slot->requests.end()));
		}

		if (!m_carried.empty())
		{
			// Carried requests are older than the ones published now, so they run
			// first - an upload and the drop that preceded it must not swap places.
			std::vector<RenderRequest> merged;
			merged.reserve(m_carried.size() + frame.requests.size());
			merged.insert(merged.end(),
			              std::make_move_iterator(m_carried.begin()),
			              std::make_move_iterator(m_carried.end()));
			merged.insert(merged.end(),
			              std::make_move_iterator(frame.requests.begin()),
			              std::make_move_iterator(frame.requests.end()));
			frame.requests = std::move(merged);
			m_carried.clear();
		}

		m_slot = std::move(frame);
	}
	m_cv.notify_one();
}

AcquireStatus FrameMailbox::Acquire(PublishedFrame& out, AcquireMode mode)
{
	std::unique_lock<std::mutex> lock(m_mutex);

	// With nothing delivered yet - and again after Invalidate() dropped the held
	// frame - Reuse has no frame to repeat and Idle has no frame to keep showing,
	// so both wait exactly like Wait. Checked under the lock: Invalidate() sets
	// the flag from another thread.
	if (mode == AcquireMode::Reuse && !m_needFresh && !m_stop && !m_wake && !m_slot.has_value())
	{
		// Reuse never blocks: with no new frame the caller redraws the one it
		// already holds. Stop and wake still outrank that, so they fall through
		// to the wait below, which returns without blocking.
		return AcquireStatus::Repeat;
	}

	// Re-read under the lock on every wakeup rather than once: Invalidate() may
	// raise the flag while an Idle caller is already parked, and from then on
	// that caller must wait for a frame like any other.
	m_cv.wait(lock, [this, mode]
	{
		// A stop or a wake always ends the wait. Otherwise only a frame does - and
		// an Idle caller rechecks after every Publish and stays parked, so it never
		// consumes a slot it does not intend to draw. The one frame it does take is
		// one carrying requests: those had to execute anyway, and leaving them in
		// the slot would starve them for as long as the viewport stays converged.
		return m_stop || m_wake ||
		       (m_slot.has_value() &&
		        (m_needFresh || mode != AcquireMode::Idle || !m_slot->requests.empty()));
	});

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

	// The predicate guarantees a frame is pending (stop and wake returned above),
	// so the move is safe. An Idle caller reaches this only for a frame carrying
	// requests, or after Invalidate() forced it back to Wait semantics.
	out = std::move(*m_slot);
	m_slot.reset();  // Moved-from optional still holds a value; empty the slot.
	m_needFresh = false;  // The caller now holds a frame it may redraw.
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

void FrameMailbox::Invalidate()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		// Dropping the slot is the point: a snapshot published before the request
		// that prompted this call describes the old state, and drawing it - even
		// once - would bake that state into a restarted accumulation. The requests
		// it carried outlive it: they are queued commands, and the very request
		// that prompted this call is waiting to run.
		if (m_slot.has_value())
		{
			m_carried.insert(m_carried.end(),
			                 std::make_move_iterator(m_slot->requests.begin()),
			                 std::make_move_iterator(m_slot->requests.end()));
			m_slot.reset();
		}

		m_needFresh = true;
		// Wake under the same mutex the consumer waits on, so the new state is
		// visible by the time a consumer draining on Interrupted looks at it.
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
