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
 * - RenderThread: Post() hands the request to the render thread, which attaches
 *   it to the next published frame so it runs before that frame is drawn.
 *
 * Ordering contract for callers on the main thread: a request posted before a
 * publish executes with that frame, before it is drawn; one posted after it
 * executes with the next published frame. Requests are never dropped - a frame
 * displaced or invalidated before the renderer took it carries its requests on
 * to the next one, in posting order.
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
