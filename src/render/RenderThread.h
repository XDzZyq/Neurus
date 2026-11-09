/**
 * @file RenderThread.h
 * @brief Dedicated thread that owns every call into the renderer.
 *
 * Once started, the render thread is the only thread that touches the device
 * queue, the swapchain, the render cache or any pass: it draws frames, runs the
 * Editor's requests and services the main thread's control messages, strictly
 * one after another. The main thread only publishes frames and posts messages,
 * and never waits on the GPU except through a future it chose to wait on (pick,
 * surface change).
 *
 * The loop, one iteration per wake:
 *
 * @code
 * PublishedFrame frame;                   // kept across iterations
 * for (;;)
 * {
 *     if (renderer.WaitForFrameSlot())    // GPU done with the previous frame
 *         DrainControl();                 // resize, surface, pick, screenshot
 *     const auto mode = ModeFor(renderer.NextWork());  // see NextWork
 *     const auto s = mailbox.Acquire(frame, mode);
 *     if (s == AcquireStatus::Stopped)    break;
 *     if (s == AcquireStatus::Interrupted) continue;
 *     if (s == AcquireStatus::Frame)      // a Repeat frame carries nothing
 *     {
 *         for (auto& request : frame.requests)
 *             renderer.Post(std::move(request));   // the tick's requests, FIFO
 *         frame.requests.clear();         // a Reuse draw must not post them again
 *     }
 *     renderer.DrawFrame(frame);          // on Repeat, the frame already held
 * }
 * renderer.WaitIdle();
 * @endcode
 *
 * Why the fence wait comes first: a control message may replace or read a
 * resource the previous frame's submission still uses, and a control is executed
 * here, so an Interrupted iteration does not draw and the loop waits for the slot
 * before draining. Requests need no such gate: the loop only queues them, and
 * DrawFrame executes them after its own fence wait.
 *
 * Why the loop asks the renderer how much work is left before it waits: once
 * progressive accumulation has converged there is nothing to sample any more,
 * and a publisher that keeps publishing would otherwise keep the GPU busy for
 * nothing. NextWork() answers what the next DrawFrame would do, and the answer
 * decides how long the thread blocks: Sample reuses the frame it holds (no new
 * snapshot needed for the remaining samples), Overlay and Retry wait for a new
 * one, Idle parks until a control message or a request-carrying frame arrives.
 *
 * Ordering guarantee (the reason a request is not posted straight to the
 * renderer): a request is queued for the next Publish, which attaches it to the
 * frame of the tick that posted it. The loop hands that frame's requests to the
 * renderer before DrawFrame, and DrawFrame drains them before it draws, so a
 * request never executes against a snapshot older than its own tick - nor does a
 * frame ever draw before the requests that belong to it. Nothing is lost on the
 * way: a frame displaced before the render thread acquired it, and one discarded
 * by Invalidate() (a resize or a surface change restarts accumulation), leave
 * their requests behind for the next publish. Only a request posted after a
 * Publish waits for the following one, which is exactly the ordering Post() and
 * Publish() establish under the posting lock.
 *
 * Lifetime: Start() once. Stop() is idempotent and the destructor calls it.
 * When the loop ends - stop or an escaped exception - the thread stops
 * accepting control messages and destroys the pending ones, breaking their
 * promises; a message posted after that is destroyed at once. A caller waiting
 * on a future therefore never hangs on a dead thread.
 *
 * Qt-free and Editor-free, standard C++ only (std::thread + explicit stop flag;
 * AppleClang's libc++ has no <stop_token>).
 */

#pragma once

#include "core/BatchQueue.h"
#include "FrameMailbox.h"
#include "IFrameRenderer.h"
#include "ProfilingData.h"
#include "PublishedFrame.h"
#include "RenderControl.h"
#include "RenderFrameSnapshot.h"
#include "requests/RenderRequestSink.h"

#include <vulkan/vulkan_raii.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace neurus {

/**
 * @brief What the render thread reports back, copied out as one value.
 */
struct RendererStatus
{
	FrameProfile profile;  ///< Profile of the last drawn frame.
	vk::Extent2D extent;   ///< Swapchain extent after the last resize/surface change.
};

/**
 * @brief Owns the render thread, its frame mailbox and its control queue.
 *
 * Borrows the renderer, which must outlive this object. Neither copyable nor
 * movable: the thread captures `this`.
 */
class RenderThread final : public RenderRequestSink
{
public:
	/** @param renderer  Borrowed; only this object calls into it once started. */
	explicit RenderThread(IFrameRenderer& renderer);

	/** @brief Stops and joins the thread if it is running. */
	~RenderThread() override;

	RenderThread(const RenderThread&)            = delete;
	RenderThread& operator=(const RenderThread&) = delete;
	RenderThread(RenderThread&&)                 = delete;
	RenderThread& operator=(RenderThread&&)      = delete;

	/**
	 * @brief Launch the thread. Call at most once.
	 *
	 * Messages and requests posted before Start() are kept and serviced by the
	 * first iteration.
	 */
	void Start();

	/**
	 * @brief Request shutdown and join. Idempotent; safe without Start().
	 *
	 * A frame being drawn finishes first; frames still pending are dropped.
	 * The renderer is idle (WaitIdle) when this returns after a started thread.
	 */
	void Stop();

	/** @return True between Start() and the end of the loop. */
	bool Running() const;

	/**
	 * @brief Hand over the newest frame, carrying the requests posted since the
	 *        previous publish (latest-wins; never blocks).
	 */
	void Publish(RenderFrameSnapshot&& snapshot);

	/**
	 * @brief Queue an Editor request for the next published frame.
	 *
	 * Appends to the posting queue that Publish() moves into the frame it hands
	 * over, so the request executes with the snapshot of the tick that posted it -
	 * before that frame is drawn, and never with an older one. Neither wakes nor
	 * invalidates anything: a request arriving between two publishes belongs to
	 * the next one, and there is no frame yet to invalidate.
	 */
	void Post(RenderRequest&& request) override;

	/** @brief Resize the swapchain; returns at once. Coalesced per batch. */
	void PostResize(uint32_t width, uint32_t height);

	/**
	 * @brief Rebuild the swapchain on @p surface.
	 * @return Satisfied once the old swapchain is gone, so the caller may then
	 *         destroy the old surface; carries the handler's exception if any,
	 *         or std::future_error (broken promise) if the thread is stopped.
	 */
	std::future<void> PostSurfaceChange(const vk::raii::SurfaceKHR& surface);

	/**
	 * @brief Read the object id under a normalised viewport position.
	 * @param u, v  In [0, 1]; clamped onto the swapchain extent by the thread.
	 * @return The id (0 = background); broken promise if the thread is stopped.
	 */
	std::future<uint32_t> PostPick(float u, float v);

	/**
	 * @brief Write a screenshot (or every attachment) between frames.
	 * @param allAttachments  False: the swapchain image. True: every attachment.
	 * @param directory       Destination root; the Application resolves it from
	 *                        the executable directory, so the renderer never
	 *                        depends on the process working directory.
	 */
	void PostScreenshot(bool allAttachments, std::filesystem::path directory);

	/** @return A copy of the latest status; callable from any thread. */
	RendererStatus Status() const;

private:
	/** @brief Thread body: the loop in the file header. */
	void Run();

	/** @brief Execute one taken batch of control messages, FIFO. */
	void DrainControl();

	/**
	 * @brief Enqueue a control message and wake, or destroy it if the loop has
	 *        ended (breaking any promise it carries).
	 * @param restartAccumulation  True when the message invalidates the held
	 *        frame (a resize or a surface change restarts accumulation), false
	 *        for messages that must neither discard nor force a frame (pick,
	 *        screenshot).
	 */
	void PostControl(RenderControl&& message, bool restartAccumulation);

	/** @brief Store the renderer's current extent into the status slot. */
	void PublishExtent();

	IFrameRenderer& m_renderer;
	FrameMailbox    m_mailbox;

	/// Control messages; pushed and destroyed under m_acceptMutex (see PostControl).
	BatchQueue<RenderControl> m_control;

	/// Guards m_accepting together with every push into m_control, so a message
	/// is either pushed before the loop's final drain or rejected after it -
	/// never stranded in the queue with a promise nobody will satisfy.
	std::mutex m_acceptMutex;
	bool       m_accepting = true;

	mutable std::mutex m_statusMutex;
	RendererStatus     m_status;   ///< Guarded by m_statusMutex.

	/// Render-thread-only: the frame being drawn; DrawFrame's context points at
	/// its snapshot. On Repeat it keeps the last one, which is what makes a Sample
	/// redraw possible without a new publish. Its requests are handed over and
	/// cleared on every Frame, so a Repeat draw posts none.
	PublishedFrame m_frame;

	/// Guards m_posted: Post() may run on any thread while Publish() takes the
	/// queue from the main one.
	std::mutex m_postMutex;

	/// @brief Requests posted since the last Publish, in posting order; moved into
	///        the frame Publish() hands over. Guarded by m_postMutex.
	std::vector<RenderRequest> m_posted;

	std::thread m_thread;
	bool        m_started = false; ///< Main-thread-only.
	std::atomic<bool> m_running{false};
};

} // namespace neurus
