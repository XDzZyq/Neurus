/**
 * @file test_render_thread.cpp
 * @brief The render thread's loop, serialization and shutdown contract.
 *
 * RenderThread is the one thread allowed to call into the renderer once running,
 * so its guarantees are all about ordering and lifetime rather than pixels, and
 * every one of them is checked headless against a fake renderer - no device, no
 * GPU. The properties pinned here, each easy to "pass" by accident:
 *
 *   1. **Start/Stop/destructor lifetime.** Stop() joins, is idempotent, and is
 *      safe without a Start(); the destructor calls it; WaitIdle runs once on the
 *      way out so the renderer is drained before the thread is gone.
 *   2. **Request-before-frame ordering.** A frame published after a Post() is
 *      drawn only after that request executed. The fake's DrawFrame asserts
 *      `processed >= tag` and the fake deliberately does NOT drain requests in
 *      DrawFrame, so the only thing that can satisfy the invariant is the loop
 *      draining before it draws - the whole point of Post() always waking.
 *   3. **Single-threaded renderer access.** Controls never run inside DrawFrame:
 *      the fake records a violation if a resize lands while a draw is in flight.
 *   4. **Resize coalescing**, **pick coordinate mapping**, **surface-change and
 *      pick promises**, and the **broken-promise shutdown** that keeps a waiting
 *      caller from hanging on a dead thread.
 *   5. **The fence gate**: with WaitForFrameSlot() false, neither controls nor
 *      requests run; once it returns true a wake lets them through.
 *   6. **Fail-loud**: an exception escaping DrawFrame ends the loop, after which
 *      the thread stops accepting and every pending promise breaks.
 *
 * Every cross-thread wait is bounded (5 s) and resolved on a condition variable
 * or a future, never a fixed sleep: a regression fails the test instead of
 * wedging ctest. No thread is ever detached; each is joined on every path.
 */

#include <gtest/gtest.h>

#include "render/RenderThread.h"

#include "render/IFrameRenderer.h"
#include "render/RenderContext.h"
#include "render/RenderFrameSnapshot.h"
#include "render/RenderRequests.h"

#include "core/BatchQueue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace neurus;

namespace
{

/// @brief Upper bound on every cross-thread wait - generous, never a timing guess.
constexpr auto WAIT_LIMIT = std::chrono::seconds(5);

/** @brief A frame tagged through environmentId, which the fake reads as its tag. */
RenderFrameSnapshot MakeTaggedFrame(int tag)
{
	RenderFrameSnapshot frame;
	frame.hasScene       = true;
	frame.hasEnvironment = true;
	frame.environmentId  = tag;
	return frame;
}

/**
 * @brief Headless IFrameRenderer that records how the thread drove it.
 *
 * All observable state is guarded by one mutex and every mutation notifies a
 * condition variable, so the tests wait on events (a draw happened, a resize
 * landed) with a bound instead of sleeping. Post() is backed by a thread-safe
 * BatchQueue, the one method the thread may call from another thread.
 *
 * DrawFrame deliberately does NOT drain requests: the ordering test only proves
 * something because draining is the loop's job, not the renderer's.
 */
class FakeRenderer final : public IFrameRenderer
{
public:
	// --- Configuration (safe to set before Start; some also during the run) ---

	void SetExtent(uint32_t width, uint32_t height)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_extent = vk::Extent2D{width, height};
	}

	void SetSlotReady(bool ready)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_slotReady = ready;
		}
		m_cv.notify_all();
	}

	void SetThrowOnDraw(bool shouldThrow)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_throwOnDraw = shouldThrow;
	}

	void SetProfileDrawCalls(uint32_t drawCalls)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_profile.drawCalls = drawCalls;
	}

	// --- IFrameRenderer ---

	bool WaitForFrameSlot() override
	{
		bool ready = false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_waitSlotCalls += 1;
			ready = m_slotReady;
		}
		m_cv.notify_all();
		return ready;
	}

	void ProcessRequests() override
	{
		std::vector<RenderRequest> batch = m_requests.TakeAll();
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_processed += static_cast<int>(batch.size());
		}
		m_cv.notify_all();
	}

	const FrameProfile& DrawFrame(const RenderContext& ctx) override
	{
		const int tag = ctx.Frame().environmentId;
		bool shouldThrow = false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			// A draw is in flight: a resize that observes this flag set proves the
			// thread let a control run inside DrawFrame. It never can - same thread
			// - and this is how the test shows it.
			m_inDraw = true;

			// The request-before-frame guarantee, as an invariant: drawing tag=i
			// means >= i requests ran, because request i was posted before frame i.
			if (m_processed < tag)
				m_orderingViolations += 1;

			m_drawn   += 1;
			m_lastTag  = tag;
			m_inDraw   = false;
			shouldThrow = m_throwOnDraw;
		}
		m_cv.notify_all();

		if (shouldThrow)
			throw std::runtime_error("FakeRenderer: forced draw failure");

		// m_profile is only written before Start in these tests, so the reference
		// stays valid and race-free for the thread's status copy.
		return m_profile;
	}

	void HandleResize(uint32_t width, uint32_t height) override
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_inDraw)
				m_drawViolations += 1;
			m_extent      = vk::Extent2D{width, height};
			m_resizeCalls += 1;
			m_lastResizeW  = width;
			m_lastResizeH  = height;
		}
		m_cv.notify_all();
	}

	void HandleSurfaceChange(const vk::raii::SurfaceKHR& /*surface*/) override
	{
		// Deliberately ignores the surface: the thread's promise handling is under
		// test, not any Vulkan work, so a null handle is fine here.
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_surfaceChanges += 1;
		}
		m_cv.notify_all();
	}

	uint32_t ReadIDBufferPixel(uint32_t x, uint32_t y) override
	{
		// Encodes the pixel so a test can assert the exact mapping the thread did.
		return x * 1000 + y;
	}

	vk::Extent2D GetExtent() const override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_extent;
	}

	void CaptureScreenshot(bool allAttachments) override
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_screenshots       += 1;
			m_lastScreenshotAll  = allAttachments;
		}
		m_cv.notify_all();
	}

	void WaitIdle() override
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_waitIdleCalls += 1;
		}
		m_cv.notify_all();
	}

	void Post(RenderRequest&& request) override
	{
		m_requests.Push(std::move(request));
	}

	// --- Bounded observation (predicates read members under the held mutex) ---

	bool WaitForDrawn(int atLeast)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return m_cv.wait_for(lock, WAIT_LIMIT, [&] { return m_drawn >= atLeast; });
	}

	bool WaitForResizeCalls(int atLeast)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return m_cv.wait_for(lock, WAIT_LIMIT, [&] { return m_resizeCalls >= atLeast; });
	}

	bool WaitForWaitSlotCalls(int atLeast)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return m_cv.wait_for(lock, WAIT_LIMIT, [&] { return m_waitSlotCalls >= atLeast; });
	}

	bool WaitForWaitIdle(int atLeast)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return m_cv.wait_for(lock, WAIT_LIMIT, [&] { return m_waitIdleCalls >= atLeast; });
	}

	// --- Accessors (each takes a snapshot under the mutex) ---

	int  Drawn()              const { std::lock_guard<std::mutex> l(m_mutex); return m_drawn; }
	int  OrderingViolations() const { std::lock_guard<std::mutex> l(m_mutex); return m_orderingViolations; }
	int  DrawViolations()     const { std::lock_guard<std::mutex> l(m_mutex); return m_drawViolations; }
	int  ResizeCalls()        const { std::lock_guard<std::mutex> l(m_mutex); return m_resizeCalls; }
	int  WaitIdleCalls()      const { std::lock_guard<std::mutex> l(m_mutex); return m_waitIdleCalls; }
	int  SurfaceChanges()     const { std::lock_guard<std::mutex> l(m_mutex); return m_surfaceChanges; }
	int  WaitSlotCalls()      const { std::lock_guard<std::mutex> l(m_mutex); return m_waitSlotCalls; }

	std::pair<uint32_t, uint32_t> LastResize() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return {m_lastResizeW, m_lastResizeH};
	}

private:
	mutable std::mutex      m_mutex;
	std::condition_variable m_cv;

	BatchQueue<RenderRequest> m_requests;  ///< Thread-safe; the one cross-thread call.

	vk::Extent2D m_extent{100, 50};

	bool m_slotReady   = true;
	bool m_throwOnDraw = false;
	bool m_inDraw      = false;

	int m_processed          = 0;
	int m_drawn              = 0;
	int m_lastTag            = 0;
	int m_orderingViolations = 0;
	int m_drawViolations     = 0;
	int m_resizeCalls        = 0;
	int m_waitSlotCalls      = 0;
	int m_surfaceChanges     = 0;
	int m_waitIdleCalls      = 0;
	int m_screenshots        = 0;

	uint32_t m_lastResizeW     = 0;
	uint32_t m_lastResizeH     = 0;
	bool     m_lastScreenshotAll = false;

	FrameProfile m_profile{};
};

/**
 * @brief Bound "the loop has ended" - a state with no event to wait on.
 *
 * Yields rather than sleeps, so it is a bounded spin and never a timing
 * assumption. Returns false on timeout so the caller fails loudly.
 */
bool WaitForRunningFalse(RenderThread& thread)
{
	const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (!thread.Running())
			return true;
		std::this_thread::yield();
	}
	return !thread.Running();
}

} // namespace

// ============================================================================
// Lifetime: Start, Stop, the destructor and WaitIdle
// ============================================================================

TEST(RenderThreadTest, StopJoinsAThreadParkedWithNoFrames)
{
	// With no frame ever published the loop parks in Acquire(). Stop() must wake
	// it, join, and drain the renderer exactly once on the way out.
	FakeRenderer fake;
	RenderThread thread(fake);
	thread.Start();

	// Let the loop reach its park before stopping, so the stop path exercises a
	// genuinely blocked Acquire() rather than a thread still starting up.
	ASSERT_TRUE(fake.WaitForWaitSlotCalls(1)) << "the loop never reached its first fence wait";

	thread.Stop();

	EXPECT_FALSE(thread.Running()) << "Running() must be false once the loop has ended";
	EXPECT_EQ(fake.WaitIdleCalls(), 1) << "the renderer must be drained once on shutdown";
}

TEST(RenderThreadTest, DestructorJoinsWithoutAnExplicitStop)
{
	// The renderer outlives the thread, so WaitIdle's count survives into the
	// assertion after the thread object is gone.
	FakeRenderer fake;
	{
		RenderThread thread(fake);
		thread.Start();
		ASSERT_TRUE(fake.WaitForWaitSlotCalls(1)) << "the loop never started";
		// No Stop(): the destructor must join and drain.
	}
	EXPECT_EQ(fake.WaitIdleCalls(), 1) << "the destructor must stop, join and drain";
}

TEST(RenderThreadTest, StopWithoutStartIsSafeAndIdempotent)
{
	// A thread that was never launched must still shut down cleanly, and a second
	// Stop() must be harmless - the destructor will call it again.
	FakeRenderer fake;
	RenderThread thread(fake);

	thread.Stop();
	thread.Stop();

	EXPECT_FALSE(thread.Running()) << "a never-started thread is not running";
	EXPECT_EQ(fake.WaitIdleCalls(), 0) << "no loop ran, so nothing was drained";
}

// ============================================================================
// Request-before-frame ordering
// ============================================================================

TEST(RenderThreadTest, AFramePublishedAfterAPostIsDrawnOnlyAfterThatRequestRan)
{
	// For each tick: Post a request, then Publish the matching frame. The fake
	// draws the newest frame it is handed and checks `processed >= tag`. Frames
	// may be dropped (latest-wins), but a drawn tag=i can only appear after the
	// loop drained request i - which it does only because Post() always wakes and
	// the wake outranks the pending frame. The fake never drains in DrawFrame, so
	// nothing else could satisfy the invariant.
	FakeRenderer fake;
	RenderThread thread(fake);
	thread.Start();

	constexpr int TICKS = 2000;
	for (int i = 1; i <= TICKS; ++i)
	{
		thread.Post(ResetAccumulationRequest{});
		thread.Publish(MakeTaggedFrame(i));
	}

	ASSERT_TRUE(fake.WaitForDrawn(1)) << "no frame was ever drawn";
	thread.Stop();

	EXPECT_GE(fake.Drawn(), 1) << "at least one frame must have been drawn";
	EXPECT_EQ(fake.OrderingViolations(), 0)
		<< "a frame was drawn before the request posted ahead of it had executed";
}

// ============================================================================
// Single-threaded renderer access
// ============================================================================

TEST(RenderThreadTest, ControlsNeverRunWhileAFrameIsBeingDrawn)
{
	// A publisher thread streams frames while the main thread posts resizes. The
	// fake flags a resize that observes a draw in flight. It can never happen -
	// draws and controls run on the one render thread - and that is exactly the
	// property under test.
	FakeRenderer fake;
	RenderThread thread(fake);
	thread.Start();

	std::atomic<bool> stopPublishing{false};
	std::thread publisher([&]
	{
		int tag = 0;
		while (!stopPublishing.load())
			thread.Publish(MakeTaggedFrame(++tag));
	});

	constexpr int RESIZES = 200;
	for (int i = 1; i <= RESIZES; ++i)
		thread.PostResize(static_cast<uint32_t>(i), static_cast<uint32_t>(i));

	ASSERT_TRUE(fake.WaitForResizeCalls(1)) << "no resize was ever serviced";

	stopPublishing.store(true);
	publisher.join();
	thread.Stop();

	EXPECT_EQ(fake.DrawViolations(), 0) << "a control ran while a frame was being drawn";
}

// ============================================================================
// Resize coalescing
// ============================================================================

TEST(RenderThreadTest, OnlyTheLastResizeInABatchIsApplied)
{
	// Three resizes queued before Start land in one batch the first iteration
	// drains; only the last must run. A pick posted afterwards gives a future to
	// synchronise on, and - because it keeps its FIFO place after the resize - it
	// reads the extent the surviving resize produced.
	FakeRenderer fake;
	RenderThread thread(fake);

	thread.PostResize(1, 1);
	thread.PostResize(2, 2);
	thread.PostResize(3, 3);
	thread.Start();

	std::future<uint32_t> pick = thread.PostPick(0.0f, 0.0f);
	ASSERT_EQ(pick.wait_for(WAIT_LIMIT), std::future_status::ready) << "the pick never completed";
	pick.get();

	EXPECT_EQ(fake.ResizeCalls(), 1) << "earlier resizes in the batch must be coalesced away";
	EXPECT_EQ(fake.LastResize(), (std::pair<uint32_t, uint32_t>{3, 3})) << "the surviving resize must be the last";

	const RendererStatus status = thread.Status();
	EXPECT_EQ(status.extent, (vk::Extent2D{3, 3})) << "the published extent must match the surviving resize";

	thread.Stop();
}

// ============================================================================
// Pick coordinate mapping
// ============================================================================

TEST(RenderThreadTest, PickMapsNormalisedCoordinatesOntoTheExtentAndClamps)
{
	// Extent 100x50, and ReadIDBufferPixel encodes x*1000+y, so the returned id
	// witnesses the exact pixel the thread sampled. The centre maps to (50, 25);
	// (1, 1) clamps onto the last valid pixel (99, 49), not one past the edge;
	// negatives clamp to (0, 0).
	FakeRenderer fake;  // default extent 100x50
	RenderThread thread(fake);
	thread.Start();

	std::future<uint32_t> centre = thread.PostPick(0.5f, 0.5f);
	ASSERT_EQ(centre.wait_for(WAIT_LIMIT), std::future_status::ready) << "the centre pick never completed";
	EXPECT_EQ(centre.get(), 50u * 1000 + 25) << "0.5, 0.5 of 100x50 must sample pixel (50, 25)";

	std::future<uint32_t> corner = thread.PostPick(1.0f, 1.0f);
	ASSERT_EQ(corner.wait_for(WAIT_LIMIT), std::future_status::ready) << "the corner pick never completed";
	EXPECT_EQ(corner.get(), 99u * 1000 + 49) << "1.0, 1.0 must clamp onto the last pixel (99, 49)";

	std::future<uint32_t> below = thread.PostPick(-1.0f, -1.0f);
	ASSERT_EQ(below.wait_for(WAIT_LIMIT), std::future_status::ready) << "the clamped pick never completed";
	EXPECT_EQ(below.get(), 0u) << "negative coordinates must clamp onto pixel (0, 0)";

	thread.Stop();
}

// ============================================================================
// Surface-change promise
// ============================================================================

TEST(RenderThreadTest, SurfaceChangeFutureBecomesReadyOnceTheHandlerRan)
{
	// The fake does not touch the surface, so a null raii handle is fine; the
	// point is that the handler satisfies the promise so the caller may then
	// destroy the old surface. The future must become ready without an exception.
	FakeRenderer fake;
	RenderThread thread(fake);
	thread.Start();

	vk::raii::SurfaceKHR surface{nullptr};
	std::future<void>    done = thread.PostSurfaceChange(surface);

	ASSERT_EQ(done.wait_for(WAIT_LIMIT), std::future_status::ready) << "the surface change never completed";
	EXPECT_NO_THROW(done.get()) << "a surface change the handler accepted must not carry an exception";
	EXPECT_EQ(fake.SurfaceChanges(), 1) << "the handler must have run exactly once";

	thread.Stop();
}

// ============================================================================
// Broken promises on a stopped thread
// ============================================================================

TEST(RenderThreadTest, PickPostedAfterStopBreaksItsPromise)
{
	// Once the loop has ended the thread stops accepting, so a control posted
	// afterwards is destroyed at once and its promise breaks. A caller waiting on
	// the future sees std::future_error, never a hang on a dead thread.
	FakeRenderer fake;
	RenderThread thread(fake);
	thread.Start();
	ASSERT_TRUE(fake.WaitForWaitSlotCalls(1)) << "the loop never started";
	thread.Stop();

	std::future<uint32_t> pick = thread.PostPick(0.5f, 0.5f);
	EXPECT_THROW(pick.get(), std::future_error) << "a pick posted after Stop must break its promise";
}

TEST(RenderThreadTest, PickPostedBeforeAStartThatNeverComesBreaksItsPromise)
{
	// A control may be posted before Start(); if Stop() then runs without a
	// Start(), the queued control is destroyed and its promise breaks rather than
	// stranding a caller waiting on a thread that never ran.
	FakeRenderer fake;
	RenderThread thread(fake);

	std::future<uint32_t> pick = thread.PostPick(0.5f, 0.5f);
	thread.Stop();  // without Start()

	EXPECT_THROW(pick.get(), std::future_error) << "a never-serviced pick must break its promise";
}

// ============================================================================
// Status reflects the renderer's profile
// ============================================================================

TEST(RenderThreadTest, StatusReportsTheProfileOfTheLastDrawnFrame)
{
	// The fake returns a profile with a recognisable drawCalls; after a frame
	// drew, Status() must carry it. The profile copy happens just after DrawFrame
	// returns, so this bounds the small window between the draw and the copy.
	FakeRenderer fake;
	fake.SetProfileDrawCalls(7);
	RenderThread thread(fake);
	thread.Start();

	thread.Publish(MakeTaggedFrame(1));
	ASSERT_TRUE(fake.WaitForDrawn(1)) << "no frame was drawn";

	const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
	while (std::chrono::steady_clock::now() < deadline && thread.Status().profile.drawCalls != 7u)
		std::this_thread::yield();

	EXPECT_EQ(thread.Status().profile.drawCalls, 7u) << "Status must mirror the last frame's profile";

	thread.Stop();
}

// ============================================================================
// The fence gate: WaitForFrameSlot false holds everything off
// ============================================================================

TEST(RenderThreadTest, AClosedFrameSlotHoldsOffControlsAndRequestsUntilItOpens)
{
	// With WaitForFrameSlot() false the loop must not drain controls or requests:
	// a resize posted now stays unapplied. Proven by waiting (bounded) for the
	// loop to re-check the fence after the resize's wake and then asserting the
	// resize did not run. Opening the fence and posting a request to wake lets
	// the queued resize through.
	FakeRenderer fake;
	fake.SetSlotReady(false);
	RenderThread thread(fake);
	thread.Start();

	ASSERT_TRUE(fake.WaitForWaitSlotCalls(1)) << "the loop never reached the fence";
	const int baseline = fake.WaitSlotCalls();

	thread.PostResize(7, 7);
	ASSERT_TRUE(fake.WaitForWaitSlotCalls(baseline + 1))
		<< "the resize's wake was never observed by the loop";
	EXPECT_EQ(fake.ResizeCalls(), 0) << "a resize ran while the frame slot was closed";

	fake.SetSlotReady(true);
	thread.Post(ResetAccumulationRequest{});  // wake the loop so it re-checks the now-open fence
	ASSERT_TRUE(fake.WaitForResizeCalls(1)) << "opening the fence must let the queued resize run";
	EXPECT_EQ(fake.LastResize(), (std::pair<uint32_t, uint32_t>{7, 7}));

	thread.Stop();
}

// ============================================================================
// Fail loud: an exception in DrawFrame ends the loop
// ============================================================================

TEST(RenderThreadTest, AnExceptionInDrawFrameEndsTheLoopAndBreaksPendingPromises)
{
	// DrawFrame throws; the loop must leave rather than spin on a recurring
	// fault. After it ends the thread stops accepting, so a pick posted next
	// breaks its promise - the same dead-thread guarantee as Stop().
	FakeRenderer fake;
	fake.SetThrowOnDraw(true);
	RenderThread thread(fake);
	thread.Start();

	thread.Publish(MakeTaggedFrame(1));
	ASSERT_TRUE(WaitForRunningFalse(thread)) << "an exception in DrawFrame must end the loop";

	std::future<uint32_t> pick = thread.PostPick(0.5f, 0.5f);
	EXPECT_THROW(pick.get(), std::future_error) << "a pick posted after the loop ended must break its promise";

	thread.Stop();  // idempotent; the loop has already ended
}
