/**
 * @file RenderRequestProcessor.h
 * @brief The renderer's request pipeline: the queue, its drain order and the
 *        executor that performs the GPU work.
 *
 * The Editor posts GPU work as owned `RenderRequest` values (RenderRequests.h)
 * through a `RenderRequestSink` (RenderRequestSink.h). This type is the other
 * end of that port: it owns the thread-safe FIFO and the executor, and drains
 * one whole batch per frame.
 *
 * Two kinds of request touch renderer state rather than GPU resources -
 * `ResetAccumulationRequest` and `RedrawOverlaysRequest` - so they are handled
 * here, before the executor. That keeps `RenderRequestExecutor` exhaustive over
 * the GPU kinds alone: adding a GPU request fails its `std::visit` until it has
 * a handler, and a state request can never reach it by accident.
 *
 * Ordering is the same contract the queue always had:
 * - within a batch, execution is strict FIFO;
 * - every request except `RedrawOverlaysRequest` restarts accumulation first,
 *   because it changes scene content or GPU resources;
 * - a request posted by a handler lands in the next `Drain()`'s batch, never
 *   mid-iteration over the one being executed.
 */

#pragma once

#include "core/BatchQueue.h"
#include "RenderRequestExecutor.h"
#include "RenderRequests.h"

#include <functional>

namespace neurus {

class RenderCache;
class UploadManager;
class GeometryPass;

/// Thread-safe FIFO of RenderRequests, drained one batch per frame.
using RenderRequestQueue = BatchQueue<RenderRequest>;

/**
 * @brief Receives, orders and executes the Editor's GPU requests.
 *
 * Borrows the render cache, the geometry pass and the upload manager (the last
 * may be null until the Application wires it in) the same way the executor
 * does. The two state callbacks are the renderer's own bits of state: the
 * sampling iteration and the overlays-stale flag.
 *
 * Non-copyable and non-movable: it keeps the queue both threads refer to.
 */
class RenderRequestProcessor
{
public:
	/**
	 * @brief Builds the queue and its executor over the renderer's resources.
	 *
	 * @param drainFrames          Waits every in-flight frame fence; forwarded to
	 *                             the executor for the handlers that free or
	 *                             replace a resource a recorded frame may read.
	 * @param cache                Render cache the executor writes.
	 * @param geometryPass         Pass owning the per-mesh pipeline build, or null.
	 * @param restartAccumulation  Restarts progressive accumulation from frame 0.
	 * @param markOverlaysDirty    Flags the overlays for one redraw over the
	 *                             converged image, without restarting accumulation.
	 */
	RenderRequestProcessor(std::function<void()> drainFrames,
	                       RenderCache& cache,
	                       GeometryPass* geometryPass,
	                       std::function<void()> restartAccumulation,
	                       std::function<void()> markOverlaysDirty);

	~RenderRequestProcessor() = default;

	RenderRequestProcessor(const RenderRequestProcessor&)            = delete;
	RenderRequestProcessor& operator=(const RenderRequestProcessor&) = delete;
	RenderRequestProcessor(RenderRequestProcessor&&)                 = delete;
	RenderRequestProcessor& operator=(RenderRequestProcessor&&)      = delete;

	/** @brief Append one request for the next Drain(). Returns at once. */
	void Post(RenderRequest&& request);

	/**
	 * @brief Execute every request posted since the last call, in FIFO order.
	 *
	 * Takes the whole batch in one swap, so a request a handler posts lands in
	 * the next Drain() instead of extending the one being walked.
	 */
	void Drain();

	/** @brief Set the upload service once the Application has created it. */
	void SetUploadManager(UploadManager* uploader);

private:
	RenderRequestQueue    m_queue;
	RenderRequestExecutor m_executor;

	/// @brief Renderer state a request can change; see the file comment.
	std::function<void()> m_restartAccumulation;
	std::function<void()> m_markOverlaysDirty;
};

} // namespace neurus
