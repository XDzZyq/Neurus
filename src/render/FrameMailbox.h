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
 * wake signal, and Acquire() reports which of four reasons returned it.
 *
 * That is the whole design in one slot and a few flags:
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
 * - **Acquire reports a reason: Stopped > Interrupted > Frame > Repeat.** Stop outranks
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
 * - **Acquire asks for only as much as the consumer needs.** A consumer that is
 *   still accumulating can redraw the frame it already holds (Reuse) instead of
 *   waiting for a new one; a consumer that has nothing left to do parks until a
 *   control message arrives and ignores published frames entirely (Idle). Both
 *   fall back to Wait until a frame has been delivered once - before the first
 *   frame there is nothing to redraw and nothing to idle on - and again after
 *   Invalidate() dropped the held frame.
 * - **Invalidate() forbids drawing what is already in the slot.** A request may
 *   change what a frame means (a restarted accumulation must not sample a
 *   camera the request replaced), so a snapshot published before the request
 *   must never be drawn after it. Invalidate() drops the pending slot, raises
 *   the wake flag and forces the next Acquire to wait for a fresh Publish.
 *
 * The canonical consumer loop:
 *
 * @code
 * RenderFrameSnapshot frame;                  // kept across iterations
 * for (;;)
 * {
 *     DrainControl();                         // the owner's control queue
 *     const auto s = mailbox.Acquire(frame, mode);  // Wait, Reuse or Idle
 *     if (s == AcquireStatus::Stopped) break;
 *     if (s == AcquireStatus::Interrupted) continue;
 *     Draw(frame);                            // on Repeat, the previously held frame
 * }
 * @endcode
 *
 * The control queue itself lives with the owner, not in the mailbox. The
 * producer-side rule is strict: enqueue the message first, THEN call Wake() -
 * or Invalidate() when the message also invalidates a published frame. Done in
 * that order, a wake racing a consumer that is about to wait can never be lost:
 * the flag is set under the same mutex the consumer waits on, and the message is
 * always visible by the time the consumer drains on Interrupted.
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
 * @brief Why Acquire() returned. Priority Stopped > Interrupted > Frame/Repeat.
 */
enum class AcquireStatus
{
	Frame,        ///< A frame was moved into the out parameter.
	Repeat,       ///< No new frame; redraw the frame the caller already holds.
	Interrupted,  ///< A wake was pending; drain the control queue and loop.
	Stopped,      ///< Shutdown requested; end the render loop.
};

/**
 * @brief How much work the consumer is asking for, and so how long it may block.
 */
enum class AcquireMode
{
	Wait,   ///< Block until a frame, a wake or a stop; a pending frame is taken.
	Reuse,  ///< Take a pending frame, else return Repeat at once - never blocks.
	Idle,   ///< Block until a wake or a stop only; a pending frame is left alone.
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
	 * @param out   Move-assigned the newest frame ONLY when the return is Frame;
	 *              left untouched on Repeat, Interrupted and Stopped.
	 * @param mode  How long the caller is willing to wait and what it can do
	 *              with a frame it already holds. See AcquireMode.
	 * @return Stopped if shutdown was requested (sticky, outranks all else),
	 *         Interrupted if a wake is pending (outranks a pending frame and
	 *         consumes the wake flag), Frame if a snapshot was handed over,
	 *         otherwise Repeat.
	 *
	 * Called by the consumer (render) thread. On Frame the slot is emptied, so
	 * the same frame is never delivered twice; Repeat leaves @p out holding
	 * whatever it held before.
	 *
	 * Wait blocks until a stop, a wake or a frame arrives. Reuse takes a pending
	 * frame and otherwise returns Repeat immediately - it never blocks. Idle
	 * blocks until a stop or a wake only, and leaves a pending frame in the slot
	 * for the next call; a Publish() wakes it but its predicate rechecks and it
	 * keeps waiting.
	 *
	 * @note Reuse and Idle behave exactly like Wait while no frame has been
	 *       delivered yet, and until a fresh frame follows Invalidate(): there
	 *       is nothing to redraw and nothing to idle on before the first frame,
	 *       and a snapshot that predates a request must not be drawn after it.
	 */
	AcquireStatus Acquire(RenderFrameSnapshot& out, AcquireMode mode = AcquireMode::Wait);

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
	 * @brief Discard the held frame and force the next Acquire to wait for a
	 *        fresh one, then wake a consumer blocked in Acquire().
	 *
	 * Callable from any thread. Use this instead of Wake() when a published
	 * snapshot may no longer be drawn - typically right after posting a request
	 * whose effect that snapshot predates. Drops any pending unconsumed frame,
	 * raises the wake flag and clears the "a frame has been delivered" state, so
	 * Reuse and Idle wait for the next Publish instead of redrawing a frame the
	 * caller must no longer draw.
	 *
	 * @note Ordering contract for the owner, exactly as for Wake(): enqueue the
	 *       control message or request FIRST, then call Invalidate().
	 */
	void Invalidate();

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

	/**
	 * @brief True until a frame has been delivered, and again after Invalidate().
	 *
	 * While set, Reuse and Idle degrade to Wait: the consumer holds no frame it
	 * may redraw. Guarded by m_mutex.
	 */
	bool m_needFresh = true;

	/// @brief Shutdown flag; outranks m_wake and m_slot. Guarded by m_mutex.
	bool m_stop = false;
};

} // namespace neurus
