/**
 * @file FrameMailbox.h
 * @brief Latest-wins handoff of one RenderFrameSnapshot, plus a wake signal.
 *
 * A dedicated render thread and the Qt main thread do not run at the same rate,
 * and only one of the two is allowed to care. The main thread owns the Scene and
 * the event loop: if it ever waits on the GPU, the whole application stutters.
 * So the handoff is deliberately lossy in one direction - the producer always
 * returns immediately, and a frame that the renderer never got to is dropped
 * rather than queued.
 *
 * The render thread blocks here and nowhere else, so this is also where it must
 * learn about control messages (resize, surface change, upload requests) posted
 * while no frame is coming - a static viewport publishes nothing, yet a resize
 * still has to be serviced promptly. Two condition variables cannot be waited on
 * atomically and timed polling wastes an idle core, so the single wait gains a
 * wake signal, and Acquire() reports which of three reasons returned it.
 *
 * That is the whole design in one slot and one flag:
 *
 * - **Publish never blocks and never grows.** One snapshot is pending at a time;
 *   a second Publish overwrites the first. A queue would turn a slow GPU into
 *   unbounded memory growth and, once bounded, into back-pressure on the UI.
 * - **A dropped frame is the correct outcome.** Snapshots are absolute state,
 *   not deltas, so the newest one makes every older one redundant - there is
 *   nothing to replay and nothing to reconcile.
 * - **Acquire blocks instead of spinning.** With no stop, no wake and no frame
 *   pending the consumer waits on a condition variable, so an idle renderer
 *   costs no CPU.
 * - **Acquire reports a reason: Stopped > Interrupted > Frame.** Stop outranks
 *   everything and is sticky - once RequestStop() is called, every later
 *   Acquire() returns Stopped even with a frame or a wake pending, because
 *   drawing one last frame on the way out serves nobody. A pending wake outranks
 *   a pending frame on purpose: a resize posted just before a frame was
 *   published must be handled before that frame is drawn, so Acquire() returns
 *   Interrupted and leaves the frame in the slot for the next call. The consumer
 *   drains its control queue on Interrupted and loops; the frame is still there.
 * - **Interrupted consumes the wake flag.** Many Wake() calls before the
 *   consumer runs coalesce into one Interrupted: the consumer drains the whole
 *   control queue each time, so one wakeup covers all of them.
 *
 * The canonical consumer loop:
 *
 * @code
 * for (;;)
 * {
 *     DrainControl();                         // the owner's control queue
 *     RenderFrameSnapshot frame;
 *     const auto s = mailbox.Acquire(frame);
 *     if (s == AcquireStatus::Stopped) break;
 *     if (s == AcquireStatus::Interrupted) continue;
 *     Draw(frame);
 * }
 * @endcode
 *
 * The control queue itself lives with the owner, not in the mailbox. The
 * producer-side rule is strict: enqueue the message first, THEN call Wake().
 * Done in that order, a Wake() racing a consumer that is about to wait can never
 * be lost - the flag is set under the same mutex the consumer waits on - and the
 * message is always visible by the time the consumer drains on Interrupted.
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
 * @brief Why Acquire() returned. Priority Stopped > Interrupted > Frame.
 */
enum class AcquireStatus
{
	Frame,        ///< A frame was moved into the out parameter.
	Interrupted,  ///< A wake was pending; drain the control queue and loop.
	Stopped,      ///< Shutdown requested; end the render loop.
};

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
	 * @brief Take the pending frame, or report why one is not being handed over.
	 * @param out  Move-assigned the newest frame ONLY when the return is Frame;
	 *             left untouched on Interrupted and Stopped.
	 * @return Stopped if shutdown was requested (sticky, outranks all else),
	 *         Interrupted if a wake is pending (outranks a pending frame and
	 *         consumes the wake flag), otherwise Frame.
	 *
	 * Called by the consumer (render) thread. On Frame the slot is emptied, so
	 * the same frame is never delivered twice. Blocks only when there is no
	 * stop, no wake and no frame; a wake already pending returns Interrupted at
	 * once without blocking.
	 */
	AcquireStatus Acquire(RenderFrameSnapshot& out);

	/**
	 * @brief Raise the wake signal so a blocked Acquire() returns Interrupted.
	 *
	 * Callable from any thread. Idempotent until consumed: repeated calls before
	 * the consumer runs collapse into a single Interrupted, since the consumer
	 * drains its whole control queue on each one.
	 *
	 * @note Ordering contract for the owner: enqueue the control message FIRST,
	 *       then call Wake(). The flag is raised under the mailbox mutex, so a
	 *       Wake() racing a consumer about to wait cannot be lost; enqueuing
	 *       before the call guarantees the message is visible when the consumer
	 *       drains on Interrupted.
	 */
	void Wake();

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

	/// @brief Wake signal; outranks m_slot but not m_stop. Guarded by m_mutex.
	bool m_wake = false;

	/// @brief Shutdown flag; outranks m_wake and m_slot. Guarded by m_mutex.
	bool m_stop = false;
};

} // namespace neurus
