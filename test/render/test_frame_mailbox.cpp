/**
 * @file test_frame_mailbox.cpp
 * @brief The handoff contract of FrameMailbox: latest-wins, never a stall, and
 *        a wake path that interrupts the wait for control messages.
 *
 * The mailbox exists for one reason first: the Qt main thread must be able to
 * publish a frame while the render thread is still busy with the previous one,
 * and it must cost the main thread nothing to do so. Two properties carry that,
 * and both are easy to "pass" by accident:
 *
 *   1. **Latest-wins.** A publish into an unconsumed slot REPLACES what was
 *      there. So every snapshot published below carries a distinguishing
 *      `activeObjectId`, and each assertion names the id it expects. A queue
 *      would hand back the oldest frame and fail; a test that only checked
 *      "some snapshot came back" would pass for a queue, which is the exact
 *      bug this type is meant to make impossible.
 *   2. **The producer never blocks.** Asserted structurally, never by timing:
 *      the publish-side tests run with no consumer in existence at all, so a
 *      `Publish` that waited for a reader would never reach the next line. No
 *      `sleep_for` guesses anywhere - the thread tests rendezvous on a
 *      condition variable and every cross-thread wait is bounded, so a missing
 *      `notify` fails the test instead of hanging the suite.
 *
 * The render thread also blocks here and nowhere else, so Acquire() must be
 * interruptible: a Wake() posted while no frame is coming has to break the wait
 * and return Interrupted, outranking a pending frame but not a stop. Those three
 * outcomes (Frame, Interrupted, Stopped) and their priority are pinned below,
 * including the coalescing of repeated Wake() into a single Interrupted and the
 * enqueue-then-Wake ordering the owner relies on.
 *
 * Two more behaviours follow from the renderer being allowed to stop drawing:
 * AcquireMode lets a consumer that is still accumulating redraw the frame it
 * already holds (Reuse, which never blocks) or park on control messages alone
 * (Idle, which ignores publishes), and Invalidate() guarantees that a frame
 * published before a request is never drawn after it. Both are pinned below,
 * including the rule that they fall back to Wait until a frame has actually been
 * delivered.
 *
 * `Publish` must also MOVE: a snapshot owns two vectors and two draw lists, so
 * a copy per frame is a per-frame allocation on the UI thread. That is pinned
 * by capturing `meshes.data()` before publishing and asserting the acquired
 * snapshot reports the same buffer address - true only if the vector's storage
 * was stolen rather than duplicated.
 *
 * Threading is standard C++ only (std::thread + mutex + condition_variable and
 * std::promise/std::future), matching ThreadPool: AppleClang's libc++ ships no
 * <stop_token>, so there is no std::jthread here.
 */

#include <gtest/gtest.h>

#include "render/FrameMailbox.h"
#include "render/RenderFrameSnapshot.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using namespace neurus;

namespace
{

/// @brief Upper bound on every cross-thread wait - generous, never a timing assumption.
constexpr auto WAIT_LIMIT = std::chrono::seconds(5);

/**
 * @brief A snapshot tagged so it can be told apart from every other snapshot.
 * @param id          Written to activeObjectId and folded into each mesh's UID.
 * @param meshCount   Mesh entries to allocate; >= 1 guarantees a non-null data().
 */
RenderFrameSnapshot MakeSnapshot(int id, size_t meshCount = 2)
{
	RenderFrameSnapshot snapshot;
	snapshot.hasScene       = true;
	snapshot.activeObjectId = id;
	snapshot.meshes.reserve(meshCount);
	for (size_t i = 0; i < meshCount; ++i)
	{
		MeshInstanceSnapshot mesh;
		mesh.objectId = id * 1000 + static_cast<int>(i);
		snapshot.meshes.push_back(mesh);
	}
	return snapshot;
}

/**
 * @brief Shared state for the two-thread tests, plus the mailbox under test.
 *
 * Held through a shared_ptr and captured by value by the consumer thread: a
 * consumer that never wakes can then be detached and reported as a failure
 * without dangling references into the test's stack.
 */
struct Handoff
{
	FrameMailbox mailbox;

	std::mutex              mutex;
	std::condition_variable cv;

	bool parked   = false;  ///< Consumer is about to enter Acquire().
	bool finished = false;  ///< Consumer returned from that Acquire().

	AcquireStatus               status       = AcquireStatus::Frame;  ///< What that Acquire() returned.
	int                         acquiredId   = 0;        ///< activeObjectId it saw, on Frame.
	const MeshInstanceSnapshot* acquiredData = nullptr;  ///< Its meshes buffer address, on Frame.

	/// Set before the consumer starts: which mode that single Acquire() asks for.
	AcquireMode mode = AcquireMode::Wait;

	/**
	 * @brief Clear the per-Acquire record so one handoff can serve several phases.
	 *
	 * The mailbox is deliberately KEPT: a later phase must run against the very
	 * mailbox the earlier phase seeded and invalidated - a fresh one would have
	 * its own needFresh state and no slot, which is how a test can pass without
	 * observing the behaviour it claims to pin. Call only with no consumer
	 * running: the fields are otherwise only written under `mutex`.
	 */
	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		parked       = false;
		finished     = false;
		status       = AcquireStatus::Frame;
		acquiredId   = 0;
		acquiredData = nullptr;
	}
};

/** @brief Wait for a predicate on the handoff, bounded. @return False on timeout. */
template <typename Pred>
bool WaitUntil(Handoff& handoff, Pred pred)
{
	std::unique_lock<std::mutex> lock(handoff.mutex);
	return handoff.cv.wait_for(lock, WAIT_LIMIT, pred);
}

/**
 * @brief Consumer body: announce, block in Acquire() once, record what came back.
 *
 * Takes the handoff by value so the thread keeps it alive on its own. The mode
 * is read from the handoff, so one body serves Wait, Reuse and Idle.
 */
void ConsumeOneFrame(std::shared_ptr<Handoff> handoff)
{
	{
		std::lock_guard<std::mutex> lock(handoff->mutex);
		handoff->parked = true;
	}
	handoff->cv.notify_all();

	RenderFrameSnapshot frame;
	const AcquireStatus status = handoff->mailbox.Acquire(frame, handoff->mode);

	{
		std::lock_guard<std::mutex> lock(handoff->mutex);
		handoff->status       = status;
		handoff->acquiredId   = (status == AcquireStatus::Frame) ? frame.activeObjectId : 0;
		handoff->acquiredData =
			(status == AcquireStatus::Frame && !frame.meshes.empty()) ? frame.meshes.data() : nullptr;
		handoff->finished = true;
	}
	handoff->cv.notify_all();
}

/**
 * @brief Nudge the scheduler so an announced consumer reaches its wait.
 *
 * Yields rather than sleeps: no duration is being assumed correct. The tests
 * that use this also repeat, so the parked case is exercised many times over.
 */
void LetConsumerReachTheWait()
{
	for (int spin = 0; spin < 128; ++spin)
		std::this_thread::yield();
}

/// @brief How long a "still blocked" probe waits before it concludes nothing happened.
constexpr auto BLOCKED_PROBE_LIMIT = std::chrono::milliseconds(100);

/**
 * @brief Bounded check that the consumer has NOT returned yet.
 *
 * A state with no event to wait on is only observable by waiting a little; the
 * flake direction is safe, since a scheduling hiccup can only make this return
 * true when the consumer really did finish. True means "it finished", i.e. a
 * failure for every caller below.
 */
bool FinishedWithin(Handoff& handoff, std::chrono::milliseconds limit)
{
	std::unique_lock<std::mutex> lock(handoff.mutex);
	return handoff.cv.wait_for(lock, limit, [&] { return handoff.finished; });
}

/**
 * @brief Start a consumer on @p handoff and wait (bounded) until it is parked.
 * @param consumer  Receives the new thread; joined by the caller on success.
 * @return False when the consumer never announced itself. The thread may then
 *         still be about to enter Acquire(), so it is detached after a
 *         RequestStop() - a detached thread parked forever on a destroyed
 *         mailbox is far worse than a failure. The handoff is a shared_ptr the
 *         consumer co-owns, so a detached consumer never dangles.
 */
bool StartAndPark(std::thread& consumer, const std::shared_ptr<Handoff>& handoff)
{
	consumer = std::thread(ConsumeOneFrame, handoff);
	if (WaitUntil(*handoff, [&] { return handoff->parked; }))
	{
		LetConsumerReachTheWait();
		return true;
	}

	handoff->mailbox.RequestStop();
	consumer.detach();
	return false;
}

} // namespace

// ============================================================================
// Latest-wins: an unconsumed frame is replaced, not queued
// ============================================================================

TEST(FrameMailboxTest, Publish_ReplacesTheUnconsumedFrameWithTheLatest)
{
	// Three frames published back to back with nobody reading. A queue would
	// hand back 1 then 2 then 3; the mailbox must hand back 3 and must have
	// dropped 1 and 2 outright - which the second half proves by publishing a
	// fourth frame and demanding *that* one, not a leftover.
	FrameMailbox mailbox;
	mailbox.Publish(MakeSnapshot(1));
	mailbox.Publish(MakeSnapshot(2));
	mailbox.Publish(MakeSnapshot(3));

	RenderFrameSnapshot newest;
	ASSERT_EQ(mailbox.Acquire(newest), AcquireStatus::Frame);
	EXPECT_EQ(newest.activeObjectId, 3) << "Acquire must yield the newest frame, not a queued one";
	ASSERT_EQ(newest.meshes.size(), 2u);
	EXPECT_EQ(newest.meshes[0].objectId, 3000) << "payload must belong to frame 3";

	mailbox.Publish(MakeSnapshot(4));
	RenderFrameSnapshot after;
	ASSERT_EQ(mailbox.Acquire(after), AcquireStatus::Frame);
	EXPECT_EQ(after.activeObjectId, 4) << "frames 1 and 2 must have been dropped, not backlogged";
}

TEST(FrameMailboxTest, Publish_LeavesNoFrameBehindAfterItIsConsumed)
{
	// The slot must empty on Acquire. Proven without touching stop: consume the
	// frame, publish a new one, and require the new id. An implementation that
	// kept the consumed frame around would answer with the stale id.
	FrameMailbox mailbox;
	mailbox.Publish(MakeSnapshot(10));

	RenderFrameSnapshot first;
	ASSERT_EQ(mailbox.Acquire(first), AcquireStatus::Frame);
	EXPECT_EQ(first.activeObjectId, 10);

	mailbox.Publish(MakeSnapshot(11));
	RenderFrameSnapshot second;
	ASSERT_EQ(mailbox.Acquire(second), AcquireStatus::Frame);
	EXPECT_EQ(second.activeObjectId, 11) << "the consumed frame must not be re-delivered";
}

TEST(FrameMailboxTest, Acquire_EmptiesTheSlotSoAReplacedFrameIsGoneNotShadowed)
{
	// The other half of latest-wins. The tests above prove the NEWEST frame
	// comes back, but a newest-first stack holding [1,2,3] would pass those just
	// as well: nothing there ever observes the slot genuinely EMPTY after a
	// drain, so "replaces" is untested as distinct from "shadows". This closes
	// that gap. Publish 1, 2, 3, drain once (getting 3), then park a consumer on
	// the mailbox and show it stays blocked: it can only block if the slot is
	// truly empty - i.e. 1 and 2 were dropped, not stacked under 3. A stack
	// would hand 2 straight back and the consumer would finish at once.
	//
	// The flake direction is safe. If the slot is empty (as it must be) the
	// parked consumer provably cannot finish without a Publish or a stop, so a
	// scheduling hiccup can only ever let `finished` flip true when it should
	// not - a false PASS of the still-blocked probe - never a false failure. The
	// RequestStop + join at the end is unconditional, so no thread is left
	// parked. A fixed duration is justified here because it bounds "still
	// blocked", a property with no event to wait on.
	auto handoff = std::make_shared<Handoff>();
	handoff->mailbox.Publish(MakeSnapshot(1));
	handoff->mailbox.Publish(MakeSnapshot(2));
	handoff->mailbox.Publish(MakeSnapshot(3));

	RenderFrameSnapshot newest;
	ASSERT_EQ(handoff->mailbox.Acquire(newest), AcquireStatus::Frame);
	EXPECT_EQ(newest.activeObjectId, 3) << "the drain must return the newest frame";

	std::thread consumer(ConsumeOneFrame, handoff);
	ASSERT_TRUE(WaitUntil(*handoff, [&] { return handoff->parked; }))
		<< "consumer never announced itself";
	LetConsumerReachTheWait();

	// With no further Publish the consumer must still be inside Acquire(). Had 1
	// or 2 survived under 3, this Acquire would have returned one of them.
	constexpr auto BLOCKED_PROBE = std::chrono::milliseconds(100);
	bool           finishedEarly = false;
	{
		std::unique_lock<std::mutex> lock(handoff->mutex);
		finishedEarly = handoff->cv.wait_for(lock, BLOCKED_PROBE, [&] { return handoff->finished; });
	}
	EXPECT_FALSE(finishedEarly)
		<< "the slot was not empty after the drain: a replaced frame was still deliverable";

	handoff->mailbox.RequestStop();
	ASSERT_TRUE(WaitUntil(*handoff, [&] { return handoff->finished; }))
		<< "RequestStop must wake the parked consumer";
	consumer.join();
	EXPECT_EQ(handoff->status, AcquireStatus::Stopped)
		<< "the consumer must have been woken by stop, not by a leftover frame";
}

// ============================================================================
// The producer never blocks
// ============================================================================

TEST(FrameMailboxTest, Publish_NeverBlocksWithNoConsumerInExistence)
{
	// No consumer thread is ever created to drain this mailbox. Every Publish
	// lands in a slot that is already full from the second one onwards, so a
	// mailbox that waited for a reader - a bounded queue, a rendezvous handoff,
	// anything back-pressured - would never return from the loop below, and the
	// assertions after it would never run. Reaching them *is* the assertion.
	//
	// The loop runs on its own thread with a bounded wait on a "done" flag
	// rather than inline, because gtest_discover_tests sets no per-test TIMEOUT:
	// a regression that blocked Publish would otherwise sit for ctest's 1500 s
	// global default - 25 minutes - before anyone saw a diagnostic. On timeout
	// the publisher is detached (it is wedged inside Publish and can never be
	// joined) and the test fails at once. The mailbox is a shared_ptr the thread
	// co-owns, and a wedged publisher only ever touches that mailbox, so a
	// detached publisher never dangles into freed storage.
	auto mailbox = std::make_shared<FrameMailbox>();

	std::mutex              mutex;
	std::condition_variable cv;
	bool                    done      = false;
	int                     published = 0;

	constexpr int PUBLISH_COUNT = 256;
	std::thread   publisher([mailbox, &mutex, &cv, &done, &published]
	{
		for (int id = 1; id <= PUBLISH_COUNT; ++id)
		{
			mailbox->Publish(MakeSnapshot(id));
			++published;
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			done = true;
		}
		cv.notify_all();
	});

	bool finished = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		finished = cv.wait_for(lock, WAIT_LIMIT, [&] { return done; });
	}
	if (!finished)
	{
		publisher.detach();
		FAIL() << "Publish blocked with no consumer draining the slot";
	}
	publisher.join();

	EXPECT_EQ(published, PUBLISH_COUNT) << "every Publish must have returned without a reader";
	EXPECT_FALSE(mailbox->StopRequested()) << "publishing must not imply stop";

	RenderFrameSnapshot frame;
	ASSERT_EQ(mailbox->Acquire(frame), AcquireStatus::Frame);
	EXPECT_EQ(frame.activeObjectId, PUBLISH_COUNT) << "only the last of the 256 frames survives";
}

// ============================================================================
// Publish moves - it must not copy the frame
// ============================================================================

TEST(FrameMailboxTest, Publish_MovesTheFrameInsteadOfCopyingIt)
{
	// A snapshot owns two vectors and two draw lists; copying one per frame is
	// a per-frame allocation on the UI thread. Each owned buffer's address is
	// the witness: it can only survive Publish -> slot -> Acquire unchanged if
	// the storage was stolen at every step. A copy anywhere reallocates and the
	// addresses differ. All four heavy members are witnessed, not just the two
	// vectors: a future hand-written copy-assignment on RenderFrameSnapshot
	// could regress debugDraw or gizmoDraw silently otherwise.
	FrameMailbox mailbox;

	RenderFrameSnapshot snapshot = MakeSnapshot(21, 4);
	snapshot.lights.push_back(LightSnapshot{});
	snapshot.debugDraw.segments.push_back(DebugSegment{});
	snapshot.gizmoDraw.segments.push_back(OverlaySegment{});
	const MeshInstanceSnapshot* meshes     = snapshot.meshes.data();
	const LightSnapshot*        lights     = snapshot.lights.data();
	const DebugSegment*         debugSegs  = snapshot.debugDraw.segments.data();
	const OverlaySegment*       gizmoSegs  = snapshot.gizmoDraw.segments.data();
	ASSERT_NE(meshes, nullptr);
	ASSERT_NE(lights, nullptr);
	ASSERT_NE(debugSegs, nullptr);
	ASSERT_NE(gizmoSegs, nullptr);

	mailbox.Publish(std::move(snapshot));

	RenderFrameSnapshot acquired;
	ASSERT_EQ(mailbox.Acquire(acquired), AcquireStatus::Frame);
	EXPECT_EQ(acquired.activeObjectId, 21);
	EXPECT_EQ(acquired.meshes.data(), meshes) << "mesh storage was copied, not moved";
	EXPECT_EQ(acquired.lights.data(), lights) << "light storage was copied, not moved";
	EXPECT_EQ(acquired.debugDraw.segments.data(), debugSegs) << "debug segment storage was copied, not moved";
	EXPECT_EQ(acquired.gizmoDraw.segments.data(), gizmoSegs) << "gizmo segment storage was copied, not moved";
}

TEST(FrameMailboxTest, Publish_MovesTheReplacementFrameToo)
{
	// Latest-wins must not quietly turn into a copy on the replacement path.
	FrameMailbox mailbox;
	mailbox.Publish(MakeSnapshot(30, 3));

	RenderFrameSnapshot         replacement = MakeSnapshot(31, 3);
	const MeshInstanceSnapshot* meshes      = replacement.meshes.data();
	ASSERT_NE(meshes, nullptr);
	mailbox.Publish(std::move(replacement));

	RenderFrameSnapshot acquired;
	ASSERT_EQ(mailbox.Acquire(acquired), AcquireStatus::Frame);
	EXPECT_EQ(acquired.activeObjectId, 31) << "the replacement must win";
	EXPECT_EQ(acquired.meshes.data(), meshes) << "replacement storage was copied, not moved";
}

// ============================================================================
// Stop - orderings and priority
// ============================================================================

TEST(FrameMailboxTest, StopRequested_TracksTheRequestAndIsIdempotent)
{
	FrameMailbox mailbox;
	EXPECT_FALSE(mailbox.StopRequested()) << "a fresh mailbox is running";

	mailbox.RequestStop();
	EXPECT_TRUE(mailbox.StopRequested());

	mailbox.RequestStop();
	EXPECT_TRUE(mailbox.StopRequested()) << "a second stop must be harmless";
}

TEST(FrameMailboxTest, Acquire_ReturnsStoppedImmediatelyWhenStopCameFirst)
{
	// Stop before anyone waits: Acquire must see the flag and return, not park
	// on a condition variable nobody will notify again. Reaching the line after
	// Acquire is part of what is being asserted.
	FrameMailbox mailbox;
	mailbox.RequestStop();

	RenderFrameSnapshot out;
	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Stopped);
	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Stopped) << "stop is sticky, not one-shot";
}

TEST(FrameMailboxTest, RequestStop_DiscardsAFramePublishedBeforeIt)
{
	// Documented shutdown policy: stop wins over a pending frame, so the render
	// thread's loop exits at once instead of drawing one more frame on the way
	// out. Pinned here because the opposite choice (drain, then stop) is equally
	// plausible to a reader. `out` must stay untouched when stop wins.
	FrameMailbox mailbox;
	mailbox.Publish(MakeSnapshot(40));
	mailbox.RequestStop();

	RenderFrameSnapshot out = MakeSnapshot(-1);
	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Stopped) << "stop must outrank the pending frame";
	EXPECT_EQ(out.activeObjectId, -1) << "out must be untouched when Acquire returns Stopped";
}

TEST(FrameMailboxTest, Stop_OutranksAPendingWakeAndAPendingFrameAndStaysStopped)
{
	// Full priority chain in one shot: a wake and a frame are both pending when
	// stop is requested, yet every Acquire must return Stopped. This is the one
	// test that pins Stopped > Interrupted > Frame together, and that stop stays
	// sticky even while a wake and a frame sit unconsumed behind it.
	FrameMailbox mailbox;
	mailbox.Wake();
	mailbox.Publish(MakeSnapshot(70));
	mailbox.RequestStop();

	RenderFrameSnapshot out;
	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Stopped)
		<< "stop outranks both a pending wake and a pending frame";
	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Stopped)
		<< "stop is sticky even with a wake and a frame still pending";
}

TEST(FrameMailboxTest, WakeAfterRequestStop_StillReturnsStopped)
{
	// A wake raised after stop must not downgrade the result to Interrupted:
	// once stop is set, nothing outranks it.
	FrameMailbox mailbox;
	mailbox.RequestStop();
	mailbox.Wake();

	RenderFrameSnapshot out;
	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Stopped) << "stop outranks a wake raised after it";
}

// ============================================================================
// Wake - interrupt the wait for a control message
// ============================================================================

TEST(FrameMailboxTest, Wake_WithNoFrameReturnsInterruptedAndLeavesOutUntouched)
{
	// The core wake contract: with no frame pending, a Wake() turns the next
	// Acquire into Interrupted, and `out` is never written. Seed `out` with a
	// recognisable value and require it survives untouched.
	FrameMailbox mailbox;
	mailbox.Wake();

	RenderFrameSnapshot         out    = MakeSnapshot(99, 3);
	const MeshInstanceSnapshot* before = out.meshes.data();
	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Interrupted) << "a pending wake interrupts Acquire";
	EXPECT_EQ(out.activeObjectId, 99) << "out must be untouched on Interrupted";
	EXPECT_EQ(out.meshes.data(), before) << "out storage must be untouched on Interrupted";
}

TEST(FrameMailboxTest, WakeThenPublish_YieldsInterruptedFirstThenTheFrame)
{
	// The resize-before-frame case: a wake posted just before a frame is
	// published must be serviced first, and the frame must survive the
	// interrupt to be delivered on the very next Acquire.
	FrameMailbox mailbox;
	mailbox.Wake();
	mailbox.Publish(MakeSnapshot(50));

	RenderFrameSnapshot out;
	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Interrupted)
		<< "a pending wake outranks the pending frame";

	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Frame)
		<< "the frame survives the interrupt and is delivered next";
	EXPECT_EQ(out.activeObjectId, 50) << "the frame left in the slot must be the published one";
}

TEST(FrameMailboxTest, MultipleWakesCoalesceIntoASingleInterrupt)
{
	// Three Wake() calls before the consumer runs must collapse into exactly
	// one Interrupted - the consumer drains its whole control queue each time,
	// so one wakeup covers them all. Proven without a call that could block: a
	// frame is published after the first Interrupted, so a correctly-drained
	// wake flag yields Frame on the next Acquire, while a surviving second wake
	// would steal that Acquire and wrongly return Interrupted.
	FrameMailbox mailbox;
	mailbox.Wake();
	mailbox.Wake();
	mailbox.Wake();

	RenderFrameSnapshot out;
	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Interrupted) << "three wakes collapse to one interrupt";

	mailbox.Publish(MakeSnapshot(60));
	EXPECT_EQ(mailbox.Acquire(out), AcquireStatus::Frame)
		<< "a second wake must not have survived the first Interrupted";
	EXPECT_EQ(out.activeObjectId, 60);
}

// ============================================================================
// Stop, Publish and Wake must each wake a consumer already blocked
// ============================================================================

TEST(FrameMailboxTest, RequestStop_WakesAnAcquireThatIsAlreadyBlocked)
{
	// A consumer parks inside Acquire() on an empty mailbox, and only then does
	// the main thread stop it. Without the notify in RequestStop the consumer
	// stays parked forever, so the wait is bounded and the thread is detached
	// on timeout - the test fails loudly instead of wedging the suite. Repeated
	// so the parked case is hit over and over rather than once by luck.
	for (int iter = 0; iter < 32; ++iter)
	{
		auto        handoff = std::make_shared<Handoff>();
		std::thread consumer(ConsumeOneFrame, handoff);

		if (!WaitUntil(*handoff, [&] { return handoff->parked; }))
		{
			// Stop first: a consumer that has not announced itself yet may still
			// reach Acquire(), and a detached thread parked there never leaves.
			handoff->mailbox.RequestStop();
			consumer.detach();
			FAIL() << "consumer never announced itself (iter " << iter << ")";
		}
		LetConsumerReachTheWait();

		handoff->mailbox.RequestStop();
		if (!WaitUntil(*handoff, [&] { return handoff->finished; }))
		{
			consumer.detach();
			FAIL() << "RequestStop did not wake the blocked Acquire (iter " << iter << ")";
		}
		consumer.join();

		EXPECT_EQ(handoff->status, AcquireStatus::Stopped)
			<< "a woken-by-stop Acquire must return Stopped (iter " << iter << ")";
		EXPECT_EQ(handoff->acquiredId, 0) << "no frame was ever published (iter " << iter << ")";
	}
}

TEST(FrameMailboxTest, Publish_WakesAnAcquireThatIsAlreadyBlockedAndHandsOverThatFrame)
{
	// The normal path, same shape: the consumer is waiting first, so delivery
	// depends on Publish's notify. The buffer address check rides along to show
	// the frame crossed the thread boundary by move, not by copy.
	for (int iter = 0; iter < 32; ++iter)
	{
		auto        handoff = std::make_shared<Handoff>();
		std::thread consumer(ConsumeOneFrame, handoff);

		if (!WaitUntil(*handoff, [&] { return handoff->parked; }))
		{
			// Stop first: a consumer that has not announced itself yet may still
			// reach Acquire(), and a detached thread parked there never leaves.
			handoff->mailbox.RequestStop();
			consumer.detach();
			FAIL() << "consumer never announced itself (iter " << iter << ")";
		}
		LetConsumerReachTheWait();

		RenderFrameSnapshot         frame  = MakeSnapshot(77, 3);
		const MeshInstanceSnapshot* meshes = frame.meshes.data();
		handoff->mailbox.Publish(std::move(frame));

		if (!WaitUntil(*handoff, [&] { return handoff->finished; }))
		{
			handoff->mailbox.RequestStop();
			consumer.detach();
			FAIL() << "Publish did not wake the blocked Acquire (iter " << iter << ")";
		}
		consumer.join();

		EXPECT_EQ(handoff->status, AcquireStatus::Frame)
			<< "the consumer must have received a frame (iter " << iter << ")";
		EXPECT_EQ(handoff->acquiredId, 77) << "wrong frame delivered (iter " << iter << ")";
		EXPECT_EQ(handoff->acquiredData, meshes) << "frame was copied across the thread (iter " << iter << ")";
	}
}

TEST(FrameMailboxTest, Wake_WakesAConsumerBlockedInAcquireOnAnotherThread)
{
	// The wake path's cross-thread guarantee: a consumer parked inside Acquire()
	// on an empty mailbox must return Interrupted when another thread calls
	// Wake(). A std::promise/future carries the status back and bounds the wait
	// at WAIT_LIMIT; on timeout the test adds a failure, requests stop to free
	// the parked consumer, and joins - so a regression fails instead of hanging
	// ctest, and the thread is joined on every path.
	FrameMailbox mailbox;

	std::promise<AcquireStatus> result;
	std::future<AcquireStatus>  future = result.get_future();

	std::thread consumer([&]
	{
		RenderFrameSnapshot frame;
		result.set_value(mailbox.Acquire(frame));
	});

	// Wake is set under the mailbox mutex, so even if it races the consumer
	// about to park it cannot be lost: whichever wins, the wait predicate sees
	// the flag. No parked handshake is needed for correctness.
	LetConsumerReachTheWait();
	mailbox.Wake();

	const bool woke = (future.wait_for(WAIT_LIMIT) == std::future_status::ready);
	if (!woke)
	{
		ADD_FAILURE() << "Wake did not wake the blocked Acquire within the time limit";
		mailbox.RequestStop();  // free the parked consumer so it can be joined
	}
	consumer.join();

	// After join the consumer has set the promise on every path.
	const AcquireStatus status = future.get();
	if (woke)
		EXPECT_EQ(status, AcquireStatus::Interrupted) << "a consumer woken by Wake must return Interrupted";
}

TEST(FrameMailboxTest, EnqueueThenWake_IsSeenByTheConsumerDrainingOnInterrupted)
{
	// The owner's ordering contract: enqueue the control message FIRST, then
	// Wake(). This proves that ordering is sufficient - the consumer, on
	// Interrupted, finds the message already in its queue. The consumer pushes
	// the value it drained into a promise; the main thread requires it saw the
	// enqueued value. Bounded wait, stop-and-join on timeout, join on every path.
	FrameMailbox mailbox;

	std::mutex       controlMutex;
	std::vector<int> controlQueue;

	std::promise<int> drained;
	std::future<int>  future = drained.get_future();

	std::thread consumer([&]
	{
		for (;;)
		{
			RenderFrameSnapshot frame;
			const AcquireStatus s = mailbox.Acquire(frame);
			if (s == AcquireStatus::Stopped)
			{
				drained.set_value(-1);  // woken by stop before any message was seen
				return;
			}
			if (s == AcquireStatus::Interrupted)
			{
				std::lock_guard<std::mutex> lock(controlMutex);
				if (!controlQueue.empty())
				{
					drained.set_value(controlQueue.front());
					return;
				}
			}
			// On Frame, or an Interrupted that found nothing, loop and wait again.
		}
	});

	{
		std::lock_guard<std::mutex> lock(controlMutex);
		controlQueue.push_back(1234);
	}
	mailbox.Wake();

	const bool ready = (future.wait_for(WAIT_LIMIT) == std::future_status::ready);
	if (!ready)
	{
		ADD_FAILURE() << "the consumer never drained the control message after enqueue-then-Wake";
		mailbox.RequestStop();  // free the parked consumer so it can be joined
	}
	consumer.join();

	const int seen = future.get();
	if (ready)
		EXPECT_EQ(seen, 1234) << "enqueue-then-Wake must make the message visible on Interrupted";
}

// ============================================================================
// Under real concurrency: never an older frame than one already consumed
// ============================================================================

TEST(FrameMailboxTest, Acquire_NeverYieldsAFrameOlderThanOneAlreadyConsumed)
{
	// A producer running flat out against a real consumer thread. Ids increase
	// monotonically as they are published, so the sequence the consumer observes
	// must be strictly increasing. What that proves is narrower than latest-wins:
	// a plain FIFO queue fed 1..500 and drained in order would also come back
	// strictly increasing - and would satisfy received.size() <= 500 too - so
	// this test does NOT discriminate a queue from a slot. Latest-wins lives in
	// the Publish_Replaces* / Acquire_Empties* tests above. What this one does
	// catch is no duplicate delivery, no out-of-order delivery, no torn or
	// garbage id, and no deadlock under genuine contention - and it hands a
	// future TSan run a clean, racy workload to inspect. How many frames get
	// dropped is scheduling-dependent and is deliberately not asserted.
	auto mailbox = std::make_shared<FrameMailbox>();

	std::mutex              mutex;
	std::condition_variable cv;
	std::vector<int>        received;

	std::thread consumer([&]
	{
		for (;;)
		{
			RenderFrameSnapshot frame;
			const AcquireStatus s = mailbox->Acquire(frame);
			if (s == AcquireStatus::Stopped)
				break;
			if (s == AcquireStatus::Interrupted)
				continue;
			{
				std::lock_guard<std::mutex> lock(mutex);
				received.push_back(frame.activeObjectId);
			}
			cv.notify_all();
		}
	});

	// Hand over one frame and wait for it, so the run below cannot finish
	// before the consumer has started and leave nothing to check.
	constexpr int PUBLISH_COUNT = 500;
	mailbox->Publish(MakeSnapshot(1));
	bool started = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		started = cv.wait_for(lock, WAIT_LIMIT, [&] { return !received.empty(); });
	}
	if (!started)
	{
		mailbox->RequestStop();
		consumer.join();
		FAIL() << "consumer never received the first frame";
	}

	for (int id = 2; id <= PUBLISH_COUNT; ++id)
		mailbox->Publish(MakeSnapshot(id));

	mailbox->RequestStop();
	consumer.join();

	std::lock_guard<std::mutex> lock(mutex);
	ASSERT_FALSE(received.empty());
	EXPECT_EQ(received.front(), 1) << "the first frame published is the first one consumed";
	EXPECT_LE(received.size(), static_cast<size_t>(PUBLISH_COUNT));
	for (size_t i = 1; i < received.size(); ++i)
		EXPECT_GT(received[i], received[i - 1])
			<< "frame " << received[i] << " arrived after " << received[i - 1];
	for (int id : received)
		EXPECT_TRUE(id >= 1 && id <= PUBLISH_COUNT) << "unknown frame id " << id;
}

// ============================================================================
// Reuse: redraw the frame already held instead of waiting for a new one
// ============================================================================

TEST(FrameMailboxTest, Acquire_ReuseReturnsRepeatWithoutBlockingWhenNoFrameIsPending)
{
	// The point of Reuse: a consumer that is still accumulating redraws what it
	// holds. Reaching the second call is the assertion - a Reuse that blocked
	// would never get here, since the slot is empty and nobody will publish.
	FrameMailbox mailbox;

	mailbox.Publish(MakeSnapshot(1));
	RenderFrameSnapshot held;
	ASSERT_EQ(mailbox.Acquire(held, AcquireMode::Reuse), AcquireStatus::Frame)
		<< "the first Reuse has no frame to repeat, so it must wait for one";
	EXPECT_EQ(held.activeObjectId, 1);

	RenderFrameSnapshot         probe  = MakeSnapshot(-1, 3);
	const MeshInstanceSnapshot* before = probe.meshes.data();
	EXPECT_EQ(mailbox.Acquire(probe, AcquireMode::Reuse), AcquireStatus::Repeat)
		<< "an empty slot must yield Repeat, not a block and not a stale frame";
	EXPECT_EQ(probe.activeObjectId, -1) << "out must be untouched on Repeat";
	EXPECT_EQ(probe.meshes.data(), before) << "out storage must be untouched on Repeat";
}

TEST(FrameMailboxTest, Acquire_ReuseTakesAPendingFrameInsteadOfRepeating)
{
	// Reuse is not "ignore the producer": a frame that is actually there is
	// taken, which is what lets accumulation follow a camera that moved.
	FrameMailbox mailbox;
	mailbox.Publish(MakeSnapshot(2));
	RenderFrameSnapshot first;
	ASSERT_EQ(mailbox.Acquire(first), AcquireStatus::Frame);

	mailbox.Publish(MakeSnapshot(3));
	RenderFrameSnapshot second;
	ASSERT_EQ(mailbox.Acquire(second, AcquireMode::Reuse), AcquireStatus::Frame)
		<< "Reuse must take a pending frame rather than repeat the held one";
	EXPECT_EQ(second.activeObjectId, 3);

	EXPECT_EQ(mailbox.Acquire(second, AcquireMode::Reuse), AcquireStatus::Repeat)
		<< "the delivery emptied the slot, so the next Reuse repeats";
}

TEST(FrameMailboxTest, Acquire_ReuseAndIdleWaitForTheVeryFirstFrame)
{
	// needFresh starts true because the consumer holds no frame yet: Reuse would
	// have nothing to redraw and Idle nothing to keep showing. Both must behave
	// exactly like Wait until the first Publish arrives.
	for (AcquireMode mode : {AcquireMode::Reuse, AcquireMode::Idle})
	{
		auto handoff = std::make_shared<Handoff>();
		handoff->mode = mode;

		std::thread consumer;
		ASSERT_TRUE(StartAndPark(consumer, handoff))
			<< "consumer never announced itself (mode " << static_cast<int>(mode) << ")";
		EXPECT_FALSE(FinishedWithin(*handoff, BLOCKED_PROBE_LIMIT))
			<< "mode " << static_cast<int>(mode) << " returned before any frame existed";

		handoff->mailbox.Publish(MakeSnapshot(4));
		ASSERT_TRUE(WaitUntil(*handoff, [&] { return handoff->finished; }))
			<< "the first Publish did not release mode " << static_cast<int>(mode);
		consumer.join();

		EXPECT_EQ(handoff->status, AcquireStatus::Frame)
			<< "the first frame must be delivered, not repeated";
		EXPECT_EQ(handoff->acquiredId, 4);
	}
}

// ============================================================================
// Idle: park until a control message, and leave published frames alone
// ============================================================================

TEST(FrameMailboxTest, Acquire_IdleIgnoresAPublishAndYieldsToAWake)
{
	// A renderer that has converged has nothing to do with a new frame, so an
	// Idle consumer must not be woken out of its wait by one - and must leave
	// the frame in the slot for whoever asks next. The blocked probe is the only
	// way to observe "still waiting"; a scheduling hiccup can only make it pass,
	// never fail.
	auto handoff  = std::make_shared<Handoff>();
	handoff->mode = AcquireMode::Idle;

	// Seed the mailbox the consumer actually uses: until it has delivered a
	// frame, needFresh makes Idle behave like Wait.
	handoff->mailbox.Publish(MakeSnapshot(1));
	RenderFrameSnapshot first;
	ASSERT_EQ(handoff->mailbox.Acquire(first), AcquireStatus::Frame) << "seed the held-frame state";

	std::thread consumer;
	ASSERT_TRUE(StartAndPark(consumer, handoff)) << "consumer never announced itself";

	handoff->mailbox.Publish(MakeSnapshot(2));
	EXPECT_FALSE(FinishedWithin(*handoff, BLOCKED_PROBE_LIMIT))
		<< "a Publish must not end an Idle wait";

	handoff->mailbox.Wake();
	ASSERT_TRUE(WaitUntil(*handoff, [&] { return handoff->finished; }))
		<< "Wake must return an Idle consumer";
	consumer.join();

	EXPECT_EQ(handoff->status, AcquireStatus::Interrupted) << "an Idle wait ends on Wake, not on a frame";
	EXPECT_EQ(handoff->acquiredId, 0) << "Idle must never take a frame";

	RenderFrameSnapshot kept;
	ASSERT_EQ(handoff->mailbox.Acquire(kept), AcquireStatus::Frame)
		<< "the published frame must still be in the slot for the next Wait";
	EXPECT_EQ(kept.activeObjectId, 2) << "Idle consumed the frame it was not going to draw";
}

TEST(FrameMailboxTest, Acquire_IdleReturnsStoppedWhenStopIsRequested)
{
	// An Idle consumer is the most deeply parked one there is - it ignores
	// publishes - so shutdown must still reach it. A frame is delivered first so
	// the wait really is an Idle one (needFresh cleared) and not the fallback to
	// Wait that a mailbox with no frame yet gets.
	auto handoff = std::make_shared<Handoff>();

	handoff->mailbox.Publish(MakeSnapshot(1));
	RenderFrameSnapshot first;
	ASSERT_EQ(handoff->mailbox.Acquire(first), AcquireStatus::Frame) << "seed the held-frame state";

	handoff->mode = AcquireMode::Idle;

	std::thread consumer;
	ASSERT_TRUE(StartAndPark(consumer, handoff)) << "consumer never announced itself";

	handoff->mailbox.RequestStop();
	ASSERT_TRUE(WaitUntil(*handoff, [&] { return handoff->finished; }))
		<< "RequestStop must release an Idle consumer";
	consumer.join();

	EXPECT_EQ(handoff->status, AcquireStatus::Stopped);
	EXPECT_EQ(handoff->acquiredId, 0) << "a stopped Acquire must not take the pending slot";
}

// ============================================================================
// Invalidate: a frame published before a request must never be drawn after it
// ============================================================================

TEST(FrameMailboxTest, Invalidate_DropsThePendingFrameSoOnlyALaterPublishIsDrawn)
{
	// The reason Invalidate exists. A snapshot published before the request
	// describes the state the request replaced, so it must not reach the
	// renderer at all - not even once, and not even if the consumer had not got
	// to it yet.
	//
	// Every step runs against ONE mailbox, the one the consumer waits on. A
	// fresh mailbox would have needFresh set and no slot, so a consumer parked
	// there would block for its own reason and the probe below would prove
	// nothing about the drop.
	auto handoff = std::make_shared<Handoff>();

	handoff->mailbox.Publish(MakeSnapshot(1));
	handoff->mailbox.Invalidate();

	RenderFrameSnapshot out = MakeSnapshot(-1);
	EXPECT_EQ(handoff->mailbox.Acquire(out), AcquireStatus::Interrupted)
		<< "Invalidate must raise the wake so the request runs before any draw";
	EXPECT_EQ(out.activeObjectId, -1) << "out must be untouched on Interrupted";

	// The dropped frame must be gone: a waiter on the same mailbox now has
	// nothing to take. Had the stale slot survived, this Acquire would return
	// frame 1 at once and the probe would fail.
	std::thread consumer;
	ASSERT_TRUE(StartAndPark(consumer, handoff)) << "consumer never announced itself";
	EXPECT_FALSE(FinishedWithin(*handoff, BLOCKED_PROBE_LIMIT))
		<< "the pre-Invalidate frame was still deliverable";

	handoff->mailbox.Publish(MakeSnapshot(2));
	ASSERT_TRUE(WaitUntil(*handoff, [&] { return handoff->finished; }))
		<< "the frame published after Invalidate was never delivered";
	consumer.join();

	EXPECT_EQ(handoff->status, AcquireStatus::Frame);
	EXPECT_EQ(handoff->acquiredId, 2) << "only a frame published after Invalidate may be drawn";
}

TEST(FrameMailboxTest, Invalidate_ForcesReuseAndIdleToWaitForAFreshFrame)
{
	// Reuse would happily redraw the held frame forever, which is exactly wrong
	// right after a request: the held frame predates it. Invalidate must demote
	// both Reuse and Idle to Wait until a fresh frame has been delivered.
	//
	// Both phases run against the one mailbox that was seeded and invalidated,
	// because that state is the whole subject: a consumer on a fresh mailbox
	// would block merely because it has never seen a frame.
	auto handoff = std::make_shared<Handoff>();

	handoff->mailbox.Publish(MakeSnapshot(1));
	RenderFrameSnapshot held;
	ASSERT_EQ(handoff->mailbox.Acquire(held, AcquireMode::Reuse), AcquireStatus::Frame);
	ASSERT_EQ(handoff->mailbox.Acquire(held, AcquireMode::Reuse), AcquireStatus::Repeat)
		<< "baseline: Reuse repeats rather than blocking";

	handoff->mailbox.Invalidate();
	EXPECT_EQ(handoff->mailbox.Acquire(held, AcquireMode::Reuse), AcquireStatus::Interrupted)
		<< "Invalidate raises the wake";

	// Reuse now waits instead of repeating.
	{
		handoff->mode = AcquireMode::Reuse;

		std::thread consumer;
		ASSERT_TRUE(StartAndPark(consumer, handoff)) << "consumer never announced itself (Reuse)";
		EXPECT_FALSE(FinishedWithin(*handoff, BLOCKED_PROBE_LIMIT))
			<< "Reuse repeated a frame that predates Invalidate";

		handoff->mailbox.Publish(MakeSnapshot(2));
		ASSERT_TRUE(WaitUntil(*handoff, [&] { return handoff->finished; }))
			<< "the fresh frame never reached the Reuse consumer";
		consumer.join();

		EXPECT_EQ(handoff->status, AcquireStatus::Frame) << "with needFresh set, Reuse waits like Wait";
		EXPECT_EQ(handoff->acquiredId, 2);
	}

	// Idle likewise behaves like Wait - it must take the fresh frame. Clear only
	// the per-Acquire record: the mailbox keeps the state phase 1 left behind.
	handoff->Reset();
	handoff->mailbox.Invalidate();
	EXPECT_EQ(handoff->mailbox.Acquire(held, AcquireMode::Idle), AcquireStatus::Interrupted);
	{
		handoff->mode = AcquireMode::Idle;

		std::thread consumer;
		ASSERT_TRUE(StartAndPark(consumer, handoff)) << "consumer never announced itself (Idle)";
		EXPECT_FALSE(FinishedWithin(*handoff, BLOCKED_PROBE_LIMIT))
			<< "Idle returned before the fresh frame existed";

		handoff->mailbox.Publish(MakeSnapshot(3));
		ASSERT_TRUE(WaitUntil(*handoff, [&] { return handoff->finished; }))
			<< "the fresh frame never reached the Idle consumer";
		consumer.join();

		EXPECT_EQ(handoff->status, AcquireStatus::Frame) << "with needFresh set, Idle waits like Wait";
		EXPECT_EQ(handoff->acquiredId, 3);
	}
}

TEST(FrameMailboxTest, Invalidate_StopStillOutranksEverything)
{
	// Invalidate sets two flags at once; neither may shield a stopped mailbox.
	// Every mode must still answer Stopped, and out must stay untouched.
	FrameMailbox mailbox;
	mailbox.Publish(MakeSnapshot(1));
	mailbox.Invalidate();
	mailbox.Publish(MakeSnapshot(2));  // a fresh frame is pending
	mailbox.RequestStop();

	for (AcquireMode mode : {AcquireMode::Wait, AcquireMode::Reuse, AcquireMode::Idle})
	{
		RenderFrameSnapshot out = MakeSnapshot(-1);
		EXPECT_EQ(mailbox.Acquire(out, mode), AcquireStatus::Stopped)
			<< "stop must outrank the fresh-frame requirement (mode " << static_cast<int>(mode) << ")";
		EXPECT_EQ(out.activeObjectId, -1) << "out must be untouched when Acquire returns Stopped";
	}
}
