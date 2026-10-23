/**
 * @file RenderRequestSink.h
 * @brief The one renderer-facing call the Editor is allowed to make.
 *
 * The Editor drives GPU state only by posting RenderRequest values (see
 * RenderRequests.h). Holding this interface instead of the renderer itself makes
 * that rule a type rule: the Editor cannot reach a draw, a resize, a readback or
 * a queue, because the pointer it holds has nothing but Post().
 *
 * Two implementations:
 * - DeferredRenderer, single-threaded: Post() enqueues and the next DrawFrame
 *   drains it on the same thread.
 * - RenderThread: Post() enqueues into the renderer and then wakes the render
 *   thread, so a request posted while no frame is coming is still serviced.
 *
 * Ordering contract for callers on the main thread: every Post() belonging to a
 * tick must happen before that tick's frame is published. The render thread
 * guarantees that a frame is drawn only after every request posted before its
 * publication has executed; a request posted after publication is only
 * guaranteed to execute before the NEXT frame.
 */

#pragma once

#include "RenderRequests.h"

namespace neurus {

/**
 * @brief Write-only inbox for Editor-posted GPU requests.
 */
class RenderRequestSink
{
public:
	virtual ~RenderRequestSink() = default;

	/**
	 * @brief Hand one request to the renderer; returns at once.
	 * @param request  Moved in, never copied.
	 */
	virtual void Post(RenderRequest&& request) = 0;

protected:
	RenderRequestSink() = default;
	RenderRequestSink(const RenderRequestSink&)            = default;
	RenderRequestSink& operator=(const RenderRequestSink&) = default;
};

} // namespace neurus
