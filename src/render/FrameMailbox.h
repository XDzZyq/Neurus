/**
 * @file FrameMailbox.h
 * @brief Latest-wins handoff of one RenderFrameSnapshot between two threads.
 *
 * A dedicated render thread and the Qt main thread do not run at the same rate,
 * and only one of the two is allowed to care. The main thread owns the Scene and
 * the event loop: if it ever waits on the GPU, the whole application stutters.
 * So the handoff is deliberately lossy in one direction - the producer always
 * returns immediately, and a frame that the renderer never got to is dropped
 * rather than queued.
 *
 * That is the whole design in one slot:
 *
 * - **Publish never blocks and never grows.** One snapshot is pending at a time;
 *   a second Publish overwrites the first. A queue would turn a slow GPU into
 *   unbounded memory growth and, once bounded, into back-pressure on the UI.
 * - **A dropped frame is the correct outcome.** Snapshots are absolute state,
 *   not deltas, so the newest one makes every older one redundant - there is
 *   nothing to replay and nothing to reconcile.
 * - **Acquire blocks instead of spinning.** With no frame pending the consumer
 *   waits on a condition variable, so an idle renderer costs no CPU.
 * - **Stop outranks a pending frame.** Once RequestStop() is called, Acquire()
 *   returns std::nullopt even if a snapshot is waiting: shutdown should end the
 *   render loop promptly, and drawing one last frame on the way out serves
 *   nobody. Stop is sticky - every later Acquire() returns nullopt too, which
 *   lets the consumer loop stay simple while the only wakeups are frames and
 *   stop. A later task that must also wake on a control message posted mid-idle
 *   will need an interruptible wait, which retires that one-condition shape.
 *
 * Single producer, single consumer by contract. Nothing here enforces that, but
 * the semantics only make sense for one of each: two consumers would race for
 * one frame, and two producers would drop each other's.
 *
 * Vulkan-free and Qt-free: it only moves RenderFrameSnapshot values, so it is
 * usable from the render layer, and testable with no device at all.
 *
 * @note std::jthread / std::stop_token would express the stop handshake more
 *       directly, but AppleClang's libc++ does not ship <stop_token>; this uses
 *       std::mutex + std::condition_variable and an explicit flag, the same way
 *       core/ThreadPool does.
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>

#include "render/RenderFrameSnapshot.h"

namespace neurus
{

/**
 * @brief One-slot, latest-wins channel carrying frames to the render thread.
 *
 * Neither copyable nor movable: it is the shared rendezvous point, referred to
 * by both threads, so it has one identity and one address for its lifetime.
 *
 * The mailbox owns no thread and joins nothing. Its owner is responsible for the
 * usual shutdown order - RequestStop(), join the consumer, then destroy the
 * mailbox - because destroying it while a consumer is parked inside Acquire()
 * destroys the mutex that consumer is waiting on.
 */
class FrameMailbox
{
public:
	FrameMailbox()  = default;
	~FrameMailbox() = default;

	FrameMailbox(const FrameMailbox&)            = delete;
	FrameMailbox& operator=(const FrameMailbox&) = delete;
	FrameMailbox(FrameMailbox&&)                 = delete;
	FrameMailbox& operator=(FrameMailbox&&)      = delete;

	/**
	 * @brief Hand over a frame, replacing any frame not yet acquired.
	 * @param snapshot  The frame; moved into the slot, never copied.
	 *
	 * Called by the producer (main) thread. Returns without waiting for the
	 * consumer, in every state - including after RequestStop(), where the frame
	 * is simply never read.
	 */
	void Publish(RenderFrameSnapshot&& snapshot);

	/**
	 * @brief Take the pending frame, waiting for one if the slot is empty.
	 * @return The newest published frame, or std::nullopt once stop is requested.
	 *
	 * Called by the consumer (render) thread. Moves the frame out, leaving the
	 * slot empty, so the same frame is never delivered twice.
	 */
	std::optional<RenderFrameSnapshot> Acquire();

	/**
	 * @brief Request shutdown and wake a consumer blocked in Acquire().
	 *
	 * Idempotent, and callable from either thread.
	 */
	void RequestStop();

	/** @return True once RequestStop() has been called. */
	bool StopRequested() const;

private:
	mutable std::mutex      m_mutex;
	std::condition_variable m_cv;

	/// @brief The single pending frame, if any. Guarded by m_mutex.
	std::optional<RenderFrameSnapshot> m_slot;

	/// @brief Shutdown flag; outranks m_slot in Acquire(). Guarded by m_mutex.
	bool m_stop = false;
};

} // namespace neurus
