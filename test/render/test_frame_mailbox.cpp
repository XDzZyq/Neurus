/**
 * @file test_frame_mailbox.cpp
 * @brief The handoff contract of FrameMailbox: latest-wins, and never a stall.
 *
 * The mailbox exists for one reason: the Qt main thread must be able to publish
 * a frame while the render thread is still busy with the previous one, and it
 * must cost the main thread nothing to do so. Two properties carry that, and
 * both are easy to "pass" by accident:
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
 * `Publish` must also MOVE: a snapshot owns two vectors and two draw lists, so
 * a copy per frame is a per-frame allocation on the UI thread. That is pinned
 * by capturing `meshes.data()` before publishing and asserting the acquired
 * snapshot reports the same buffer address - true only if the vector's storage
 * was stolen rather than duplicated.
 *
 * Threading is standard C++ only (std::thread + mutex + condition_variable),
 * matching ThreadPool: AppleClang's libc++ ships no <stop_token>, so there is
 * no std::jthread here.
 */

#include <gtest/gtest.h>

#include "render/FrameMailbox.h"
#include "render/RenderFrameSnapshot.h"

#include <chrono>
#include <condition_variable>
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
	bool stopped  = false;  ///< That Acquire() returned nullopt.

	int                         acquiredId   = 0;        ///< activeObjectId it saw.
	const MeshInstanceSnapshot* acquiredData = nullptr;  ///< Its meshes buffer address.
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
 * Takes the handoff by value so the thread keeps it alive on its own.
 */
void ConsumeOneFrame(std::shared_ptr<Handoff> handoff)
{
	{
		std::lock_guard<std::mutex> lock(handoff->mutex);
		handoff->parked = true;
	}
	handoff->cv.notify_all();

	std::optional<RenderFrameSnapshot> frame = handoff->mailbox.Acquire();

	{
		std::lock_guard<std::mutex> lock(handoff->mutex);
		handoff->stopped      = !frame.has_value();
		handoff->acquiredId   = frame ? frame->activeObjectId : 0;
		handoff->acquiredData = (frame && !frame->meshes.empty()) ? frame->meshes.data() : nullptr;
		handoff->finished     = true;
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

	std::optional<RenderFrameSnapshot> newest = mailbox.Acquire();
	ASSERT_TRUE(newest.has_value());
	EXPECT_EQ(newest->activeObjectId, 3) << "Acquire must yield the newest frame, not a queued one";
	ASSERT_EQ(newest->meshes.size(), 2u);
	EXPECT_EQ(newest->meshes[0].objectId, 3000) << "payload must belong to frame 3";

	mailbox.Publish(MakeSnapshot(4));
	std::optional<RenderFrameSnapshot> after = mailbox.Acquire();
	ASSERT_TRUE(after.has_value());
	EXPECT_EQ(after->activeObjectId, 4) << "frames 1 and 2 must have been dropped, not backlogged";
}

TEST(FrameMailboxTest, Publish_LeavesNoFrameBehindAfterItIsConsumed)
{
	// The slot must empty on Acquire. Proven without touching stop: consume the
	// frame, publish a new one, and require the new id. An implementation that
	// kept the consumed frame around would answer with the stale id.
	FrameMailbox mailbox;
	mailbox.Publish(MakeSnapshot(10));

	std::optional<RenderFrameSnapshot> first = mailbox.Acquire();
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first->activeObjectId, 10);

	mailbox.Publish(MakeSnapshot(11));
	std::optional<RenderFrameSnapshot> second = mailbox.Acquire();
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(second->activeObjectId, 11) << "the consumed frame must not be re-delivered";
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
	// parked. This is the one place in the file where a fixed duration is
	// justified: it bounds "still blocked", a property with no event to wait on.
	auto handoff = std::make_shared<Handoff>();
	handoff->mailbox.Publish(MakeSnapshot(1));
	handoff->mailbox.Publish(MakeSnapshot(2));
	handoff->mailbox.Publish(MakeSnapshot(3));

	std::optional<RenderFrameSnapshot> newest = handoff->mailbox.Acquire();
	ASSERT_TRUE(newest.has_value());
	EXPECT_EQ(newest->activeObjectId, 3) << "the drain must return the newest frame";

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
	EXPECT_TRUE(handoff->stopped) << "the consumer must have been woken by stop, not by a leftover frame";
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

	std::optional<RenderFrameSnapshot> frame = mailbox->Acquire();
	ASSERT_TRUE(frame.has_value());
	EXPECT_EQ(frame->activeObjectId, PUBLISH_COUNT) << "only the last of the 256 frames survives";
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

	std::optional<RenderFrameSnapshot> acquired = mailbox.Acquire();
	ASSERT_TRUE(acquired.has_value());
	EXPECT_EQ(acquired->activeObjectId, 21);
	EXPECT_EQ(acquired->meshes.data(), meshes) << "mesh storage was copied, not moved";
	EXPECT_EQ(acquired->lights.data(), lights) << "light storage was copied, not moved";
	EXPECT_EQ(acquired->debugDraw.segments.data(), debugSegs) << "debug segment storage was copied, not moved";
	EXPECT_EQ(acquired->gizmoDraw.segments.data(), gizmoSegs) << "gizmo segment storage was copied, not moved";
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

	std::optional<RenderFrameSnapshot> acquired = mailbox.Acquire();
	ASSERT_TRUE(acquired.has_value());
	EXPECT_EQ(acquired->activeObjectId, 31) << "the replacement must win";
	EXPECT_EQ(acquired->meshes.data(), meshes) << "replacement storage was copied, not moved";
}

// ============================================================================
// Stop - both orderings
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

TEST(FrameMailboxTest, Acquire_ReturnsNulloptImmediatelyWhenStopCameFirst)
{
	// Stop before anyone waits: Acquire must see the flag and return, not park
	// on a condition variable nobody will notify again. Reaching the line after
	// Acquire is part of what is being asserted.
	FrameMailbox mailbox;
	mailbox.RequestStop();

	EXPECT_FALSE(mailbox.Acquire().has_value());
	EXPECT_FALSE(mailbox.Acquire().has_value()) << "stop is sticky, not one-shot";
}

TEST(FrameMailboxTest, RequestStop_DiscardsAFramePublishedBeforeIt)
{
	// Documented shutdown policy: stop wins over a pending frame, so the render
	// thread's `while (auto frame = Acquire())` loop exits at once instead of
	// drawing one more frame on the way out. Pinned here because the opposite
	// choice (drain, then stop) is equally plausible to a reader.
	FrameMailbox mailbox;
	mailbox.Publish(MakeSnapshot(40));
	mailbox.RequestStop();

	EXPECT_FALSE(mailbox.Acquire().has_value()) << "stop must outrank the pending frame";
}

// ============================================================================
// Stop and publish must wake a consumer that is already blocked
// ============================================================================

TEST(FrameMailboxTest, RequestStop_WakesAnAcquireThatIsAlreadyBlocked)
{
	// The other ordering, and the one that actually needs the notify: a
	// consumer parks inside Acquire() on an empty mailbox, and only then does
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

		EXPECT_TRUE(handoff->stopped) << "a woken-by-stop Acquire must return nullopt (iter " << iter << ")";
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

		EXPECT_FALSE(handoff->stopped) << "the consumer must have received a frame (iter " << iter << ")";
		EXPECT_EQ(handoff->acquiredId, 77) << "wrong frame delivered (iter " << iter << ")";
		EXPECT_EQ(handoff->acquiredData, meshes) << "frame was copied across the thread (iter " << iter << ")";
	}
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
		while (std::optional<RenderFrameSnapshot> frame = mailbox->Acquire())
		{
			{
				std::lock_guard<std::mutex> lock(mutex);
				received.push_back(frame->activeObjectId);
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
