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
 *      pick promises**, the **screenshot request carrying its destination
 *      directory**, and the **broken-promise shutdown** that keeps a waiting
 *      caller from hanging on a dead thread.
 *   5. **The fence gate**: with WaitForFrameSlot() false, neither controls nor
 *      requests run; once it returns true a wake lets them through.
 *   6. **Fail-loud**: an exception escaping DrawFrame ends the loop, after which
 *      the thread stops accepting and every pending promise breaks.
 *   7. **Stop rendering after convergence**: the loop asks the renderer what the
 *      next draw is worth and waits accordingly - reusing the frame it holds
 *      while accumulating, waiting for a fresh one for an overlay redraw or a
 *      retry, and parking once converged - while a frame published before a
 *      request is never drawn after it.
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
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace neurus;

namespace
{

/// @brief Upper bound on every cross-thread wait - generous, never a timing guess.
constexpr auto WAIT_LIMIT = std::chrono::seconds(5);

/**
 * @brief Bound on "nothing more happened", the negative assertions below.
 *
 * A state with no event to wait on can only be observed by waiting a little and
 * checking it did not occur; the direction of the resulting flake is safe - a
 * scheduling hiccup can make the probe pass when it should have failed, never
 * the reverse - and it is short so the suite stays fast.
 */
constexpr auto STABLE_PROBE = std::chrono::milliseconds(100);

/**
 * @brief A frame tagged through environmentId, which the fake reads as its tag.
 *
 * The ordering invariant in the fake is `processed >= tag`, so a test that has
 * posted requests must tag its frames with at least the number posted so far.
 */
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
 *
 * It also models accumulation closely enough to script the loop: `iteration`
 * counts scene samples, `samplingCount` is how many it takes to converge, a
 * RedrawOverlaysRequest marks overlays stale without restarting the count, and
 * every other request (plus a resize or surface change) restarts it. That makes
 * NextWork() behave like the real renderer's - Sample, Overlay, Idle, Retry - so
 * the tests below can check what the loop does with each answer.
 */
class FakeRenderer final : public IFrameRenderer
{
public:
	/**
	 * @brief One DrawFrame call, kept so a test can inspect what was drawn when.
	 *
	 * `processed` is the number of requests the loop had executed before this
	 * draw, which is how a test tells a post-request draw from a stale one.
	 */
	struct DrawRecord
	{
		int  tag         = 0;      ///< environmentId of the frame handed over.
		int  processed   = 0;      ///< Requests executed before this call.
		bool drewNothing = false;  ///< DrawFrame reported Retry: nothing drawn.
	};

	// --- Configuration (safe to set before Start; some also during the run) ---

	void SetExtent(uint32_t width, uint32_t height)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_extent = vk::Extent2D{width, height};
	}

	/** @brief Scene samples needed to converge; 0 converges without any. */
	void SetSamplingCount(int count)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_samplingCount = count;
	}

	/** @brief Make DrawFrame draw nothing, the way a missing camera does. */
	void SetDrawNothing(bool drawNothing)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_drawNothing = drawNothing;
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
			for (const RenderRequest& request : batch)
			{
				if (std::holds_alternative<RedrawOverlaysRequest>(request))
				{
					// Overlays only: the converged scene image stays, so the
					// accumulation count is deliberately left alone.
					m_overlayDirty = true;
				}
				else
				{
					// Every other request restarts accumulation, as
					// IFrameRenderer::NextWork documents. A scene draw redraws
					// the overlays too, so the stale flag goes with it.
					m_iteration    = 0;
					m_overlayDirty = false;
				}
			}
		}
		m_cv.notify_all();
	}

	FrameWork NextWork() const override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		// A draw that produced nothing has to be retried, but only once a frame
		// gives the renderer something to draw - otherwise the loop spins.
		if (m_lastDrewNothing)
			return FrameWork::Retry;
		if (m_iteration < m_samplingCount)
			return FrameWork::Sample;
		if (m_overlayDirty)
			return FrameWork::Overlay;
		return FrameWork::Idle;
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

			m_drawCalls += 1;
			m_draws.push_back(DrawRecord{tag, m_processed, m_drawNothing});

			if (!m_drawNothing)
			{
				m_drawn += 1;
				if (m_iteration < m_samplingCount)
					m_iteration += 1;  // one scene sample of the accumulation
				else
					m_overlayDirty = false;  // an overlay-only redraw
			}
			// A draw that produced nothing must not count as a sample: NextWork
			// has to keep saying Retry until a new frame arrives.
			m_lastDrewNothing = m_drawNothing;
			m_lastTag         = tag;
			m_inDraw          = false;
			shouldThrow       = m_throwOnDraw;
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
			// A new swapchain restarts accumulation from scratch.
			m_iteration    = 0;
			m_overlayDirty = false;
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
			// Same reason as a resize: the old accumulation is meaningless.
			m_iteration    = 0;
			m_overlayDirty = false;
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

	void CaptureScreenshot(bool allAttachments,
	                       const std::filesystem::path& directory) override
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_screenshots       += 1;
			m_lastScreenshotAll  = allAttachments;
			m_lastScreenshotDir  = directory;
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

	/**
	 * @brief Bounded wait for a draw count, for use as a "stays put" probe.
	 *
	 * A short limit deliberately: the caller asserts the NEGATIVE, so the return
	 * value is a failure report rather than a synchronisation point.
	 */
	bool WaitForDrawnWithin(int atLeast, std::chrono::milliseconds limit)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return m_cv.wait_for(lock, limit, [&] { return m_drawn >= atLeast; });
	}

	bool WaitForDrawCalls(int atLeast)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return m_cv.wait_for(lock, WAIT_LIMIT, [&] { return m_drawCalls >= atLeast; });
	}

	bool WaitForDrawCallsWithin(int atLeast, std::chrono::milliseconds limit)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return m_cv.wait_for(lock, limit, [&] { return m_drawCalls >= atLeast; });
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

	bool WaitForScreenshots(int atLeast)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return m_cv.wait_for(lock, WAIT_LIMIT, [&] { return m_screenshots >= atLeast; });
	}

	// --- Accessors (each takes a snapshot under the mutex) ---

	int  Drawn()              const { std::lock_guard<std::mutex> l(m_mutex); return m_drawn; }
	int  DrawCalls()          const { std::lock_guard<std::mutex> l(m_mutex); return m_drawCalls; }
	int  SampledIteration()   const { std::lock_guard<std::mutex> l(m_mutex); return m_iteration; }
	int  OrderingViolations() const { std::lock_guard<std::mutex> l(m_mutex); return m_orderingViolations; }
	int  DrawViolations()     const { std::lock_guard<std::mutex> l(m_mutex); return m_drawViolations; }
	int  ResizeCalls()        const { std::lock_guard<std::mutex> l(m_mutex); return m_resizeCalls; }
	int  WaitIdleCalls()      const { std::lock_guard<std::mutex> l(m_mutex); return m_waitIdleCalls; }
	int  SurfaceChanges()     const { std::lock_guard<std::mutex> l(m_mutex); return m_surfaceChanges; }
	int  WaitSlotCalls()      const { std::lock_guard<std::mutex> l(m_mutex); return m_waitSlotCalls; }

	/** @brief Every DrawFrame call, in order. */
	std::vector<DrawRecord> Draws() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_draws;
	}

	std::pair<uint32_t, uint32_t> LastResize() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return {m_lastResizeW, m_lastResizeH};
	}

	int Screenshots() const { std::lock_guard<std::mutex> l(m_mutex); return m_screenshots; }

	bool LastScreenshotAll() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_lastScreenshotAll;
	}

	std::filesystem::path LastScreenshotDir() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_lastScreenshotDir;
	}

private:
	mutable std::mutex      m_mutex;
	std::condition_variable m_cv;

	BatchQueue<RenderRequest> m_requests;  ///< Thread-safe; the one cross-thread call.

	vk::Extent2D m_extent{100, 50};

	bool m_slotReady   = true;
	bool m_throwOnDraw = false;
	bool m_inDraw      = false;

	/// @brief Draw nothing on every call, as a missing camera or a failed acquire does.
	bool m_drawNothing = false;

	/// @brief True when the last DrawFrame call drew nothing; drives NextWork's Retry.
	bool m_lastDrewNothing = false;

	/// @brief Scene samples taken since the last accumulation restart.
	int m_iteration = 0;

	/// @brief Scene samples it takes to converge. One keeps a publish worth one draw.
	int m_samplingCount = 1;

	/// @brief Overlays are stale and want one redraw over the converged image.
	bool m_overlayDirty = false;

	int m_processed          = 0;
	int m_drawn              = 0;
	int m_drawCalls          = 0;
	int m_lastTag            = 0;
	int m_orderingViolations = 0;
	int m_drawViolations     = 0;
	int m_resizeCalls        = 0;
	int m_waitSlotCalls      = 0;
	int m_surfaceChanges     = 0;
	int m_waitIdleCalls      = 0;
	int m_screenshots        = 0;

	std::vector<DrawRecord> m_draws;

	uint32_t m_lastResizeW     = 0;
	uint32_t m_lastResizeH     = 0;
	bool     m_lastScreenshotAll = false;

	/// @brief Directory the renderer was asked to write into, delivered as-is.
	std::filesystem::path m_lastScreenshotDir;

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

// ============================================================================
// Stop rendering after convergence: the loop follows IFrameRenderer::NextWork
// ============================================================================

TEST(RenderThreadTest, ConvergedAccumulationDrawsExactlyTheSamplingCount)
{
	// One publish, then the renderer keeps sampling the frame it already holds
	// (NextWork says Sample, so the loop asks the mailbox for Reuse) until the
	// accumulation converges. After that there is nothing left to do, so the
	// thread must park instead of asking the main thread for frames forever.
	// Frame 1 (a tag of 0) is published before any request, so it gets tag 0.
	FakeRenderer fake;
	fake.SetSamplingCount(3);
	RenderThread thread(fake);
	thread.Start();

	thread.Publish(MakeTaggedFrame(0));
	ASSERT_TRUE(fake.WaitForDrawn(3)) << "accumulation must keep sampling the held frame";
	EXPECT_FALSE(fake.WaitForDrawnWithin(4, STABLE_PROBE))
		<< "a converged renderer kept drawing without a new frame";

	EXPECT_EQ(fake.Drawn(), 3) << "exactly sampling-count frames, no more";
	EXPECT_EQ(fake.SampledIteration(), 3) << "the accumulation must have converged, not overflowed";
	EXPECT_EQ(fake.OrderingViolations(), 0);

	thread.Stop();
}

TEST(RenderThreadTest, APostedRequestRestartsAccumulation)
{
	// Convergence is not final: a request that is not an overlay redraw restarts
	// the accumulation, and the next published frame must be sampled from scratch
	// - the full sampling count again, not one frame.
	FakeRenderer fake;
	fake.SetSamplingCount(3);
	RenderThread thread(fake);
	thread.Start();

	thread.Publish(MakeTaggedFrame(0));
	ASSERT_TRUE(fake.WaitForDrawn(3)) << "the first accumulation never converged";
	ASSERT_FALSE(fake.WaitForDrawnWithin(4, STABLE_PROBE)) << "the thread was not idle before the request";

	thread.Post(ResetAccumulationRequest{});
	thread.Publish(MakeTaggedFrame(1));  // one request posted, so tag 1

	ASSERT_TRUE(fake.WaitForDrawn(6)) << "a posted request must restart accumulation";
	EXPECT_FALSE(fake.WaitForDrawnWithin(7, STABLE_PROBE)) << "the restarted accumulation never converged";

	EXPECT_EQ(fake.Drawn(), 6) << "two full accumulations, nothing in between";
	EXPECT_EQ(fake.SampledIteration(), 3);
	EXPECT_EQ(fake.OrderingViolations(), 0)
		<< "a frame was drawn before the request that restarted accumulation";

	thread.Stop();
}

TEST(RenderThreadTest, AnOverlayRedrawDrawsOnceMoreWithoutRestartingAccumulation)
{
	// A RedrawOverlaysRequest is the one request that must NOT restart
	// accumulation: it redraws the overlays over the converged image, once.
	// NextWork then answers Overlay - which waits for a fresh frame, because the
	// request always arrives with an invalidate - and after that one draw it goes
	// back to Idle.
	FakeRenderer fake;
	fake.SetSamplingCount(3);
	RenderThread thread(fake);
	thread.Start();

	thread.Publish(MakeTaggedFrame(0));
	ASSERT_TRUE(fake.WaitForDrawn(3)) << "the accumulation never converged";
	ASSERT_FALSE(fake.WaitForDrawnWithin(4, STABLE_PROBE)) << "the thread was not idle before the request";

	thread.Post(RedrawOverlaysRequest{});
	thread.Publish(MakeTaggedFrame(1));  // one request posted, so tag 1

	ASSERT_TRUE(fake.WaitForDrawn(4)) << "the overlay redraw never happened";
	EXPECT_FALSE(fake.WaitForDrawnWithin(5, STABLE_PROBE))
		<< "an overlay redraw restarted accumulation or repeated itself";

	EXPECT_EQ(fake.Drawn(), 4) << "exactly one extra draw";
	EXPECT_EQ(fake.SampledIteration(), 3)
		<< "the overlay draw was counted as a scene sample - accumulation restarted";
	EXPECT_EQ(fake.OrderingViolations(), 0);

	thread.Stop();
}

TEST(RenderThreadTest, AFramePublishedBeforeAPostIsNeverDrawnAfterIt)
{
	// Why Post() invalidates instead of merely waking. Converge first, so the
	// thread sits in Idle holding frame A in its own storage; then post a request
	// and publish B. A snapshot taken before the request describes the camera the
	// request replaced, so drawing it - even once - would bake that camera into
	// every sample of the restarted accumulation, permanently.
	FakeRenderer fake;
	fake.SetSamplingCount(3);
	RenderThread thread(fake);
	thread.Start();

	thread.Publish(MakeTaggedFrame(0));  // A: published before any request
	ASSERT_TRUE(fake.WaitForDrawn(3)) << "the accumulation never converged";
	ASSERT_FALSE(fake.WaitForDrawnWithin(4, STABLE_PROBE)) << "the thread was not idle before the request";

	thread.Post(ResetAccumulationRequest{});
	thread.Publish(MakeTaggedFrame(1));  // B: published after the request

	ASSERT_TRUE(fake.WaitForDrawn(6)) << "the restarted accumulation never drew from the new frame";
	EXPECT_FALSE(fake.WaitForDrawnWithin(7, STABLE_PROBE));

	const std::vector<FakeRenderer::DrawRecord> draws = fake.Draws();
	ASSERT_EQ(draws.size(), 6u);
	for (size_t i = 3; i < draws.size(); ++i)
	{
		EXPECT_EQ(draws[i].tag, 1)
			<< "draw " << i << " sampled the frame published before the request";
		EXPECT_EQ(draws[i].processed, 1) << "draw " << i << " ran before the request executed";
	}
	EXPECT_EQ(fake.OrderingViolations(), 0);

	thread.Stop();
}

TEST(RenderThreadTest, ADrawThatProducedNothingWaitsInsteadOfSpinning)
{
	// No camera: DrawFrame draws nothing and the renderer reports Retry. The loop
	// must wait for a new frame rather than retry in a tight loop - a retry with
	// nothing new to draw cannot succeed, and spinning here would peg the core.
	// Attempts (not drawn frames) are what must stay flat.
	FakeRenderer fake;
	fake.SetSamplingCount(3);
	fake.SetDrawNothing(true);
	RenderThread thread(fake);
	thread.Start();

	thread.Publish(MakeTaggedFrame(0));
	ASSERT_TRUE(fake.WaitForDrawCalls(1)) << "the thread never attempted a draw";
	EXPECT_FALSE(fake.WaitForDrawCallsWithin(2, STABLE_PROBE))
		<< "Retry spun instead of waiting for a new frame";

	thread.Publish(MakeTaggedFrame(0));
	ASSERT_TRUE(fake.WaitForDrawCalls(2)) << "a new frame must let the renderer retry";
	EXPECT_FALSE(fake.WaitForDrawCallsWithin(3, STABLE_PROBE)) << "one retry per frame, not a loop";

	EXPECT_EQ(fake.Drawn(), 0) << "nothing was ever actually drawn";

	thread.Stop();
}

TEST(RenderThreadTest, PickIsAnsweredWhileTheRendererIsIdle)
{
	// An idle renderer is parked in the mailbox, not gone: a control message
	// still has to reach it and satisfy its promise. The pick must not force a
	// frame either - the thread returns to Idle with the draw count unchanged.
	FakeRenderer fake;
	fake.SetSamplingCount(3);
	RenderThread thread(fake);
	thread.Start();

	thread.Publish(MakeTaggedFrame(0));
	ASSERT_TRUE(fake.WaitForDrawn(3)) << "the accumulation never converged";
	ASSERT_FALSE(fake.WaitForDrawnWithin(4, STABLE_PROBE)) << "the thread was not idle before the pick";

	std::future<uint32_t> pick = thread.PostPick(0.5f, 0.5f);
	ASSERT_EQ(pick.wait_for(WAIT_LIMIT), std::future_status::ready)
		<< "an idle render thread did not service a pick";
	EXPECT_EQ(pick.get(), 50u * 1000 + 25) << "the pick must still map onto the extent";

	EXPECT_FALSE(fake.WaitForDrawnWithin(4, STABLE_PROBE)) << "a pick forced a frame";
	EXPECT_EQ(fake.Drawn(), 3);

	thread.Stop();
}

TEST(RenderThreadTest, PostScreenshotDeliversTheFlagAndTheDirectoryUnchanged)
{
	// The directory is the Application's answer to "where does Neurus live" - the
	// renderer must receive it verbatim, or screenshots fall back into a working
	// directory the user does not control (the bug: "screenshots/..." resolved
	// against the CWD). The flag picks the swapchain image or the full dump, so
	// both requests are checked.
	FakeRenderer fake;
	RenderThread thread(fake);
	thread.Start();

	const std::filesystem::path plainDir = std::filesystem::path("exe dir") / "screenshots";
	const std::filesystem::path dumpDir  = std::filesystem::path("exe dir") / "dump";

	thread.PostScreenshot(false, plainDir);
	ASSERT_TRUE(fake.WaitForScreenshots(1)) << "the plain screenshot never reached the renderer";
	EXPECT_FALSE(fake.LastScreenshotAll()) << "PostScreenshot(false) asked for more than the image";
	EXPECT_EQ(fake.LastScreenshotDir().string(), plainDir.string())
		<< "the render thread must not rewrite the directory it was given";

	thread.PostScreenshot(true, dumpDir);
	ASSERT_TRUE(fake.WaitForScreenshots(2)) << "the attachment dump never reached the renderer";
	EXPECT_TRUE(fake.LastScreenshotAll()) << "PostScreenshot(true) did not request every attachment";
	EXPECT_EQ(fake.LastScreenshotDir().string(), dumpDir.string());

	EXPECT_EQ(fake.Screenshots(), 2) << "each request must be serviced exactly once";

	thread.Stop();
}
