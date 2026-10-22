/**
 * @file RenderRequestQueue.h
 * @brief Thread-safe FIFO inbox of RenderRequests the renderer drains per frame.
 *
 * The Editor posts GPU work as values (see RenderRequests.h) and the renderer
 * executes them itself, so the two sides need one point of handoff. This is it:
 * a mutex-guarded vector that Post() appends to and TakeAll() swaps out whole.
 *
 * TakeAll() swapping the entire batch out, rather than popping one at a time, is
 * the ordering guarantee that matters: a request posted while a taken batch is
 * executing lands in the next batch, never mid-iteration, so a handler can post
 * a follow-up request without the executor tripping over a container it is
 * walking. Within a batch the order is strict FIFO - append order is execution
 * order - which is the contract RenderRequests.h relies on (an upload posted
 * after DropSceneResourcesRequest lands in the new scene).
 *
 * The mutex is here for the render thread that is coming: today DrawFrame drains
 * on the same thread the Editor posts from, so nothing contends, but once the
 * drain moves onto a dedicated render thread Post() (main thread) and TakeAll()
 * (render thread) run concurrently and the lock is what keeps the vector intact.
 *
 * @note Enqueue-then-Wake, the same ordering contract FrameMailbox documents: a
 *       future owner that parks the render thread in FrameMailbox must call
 *       FrameMailbox::Wake() AFTER Post() returns, never before, so a wake
 *       racing a consumer about to block cannot be lost and the posted request
 *       is always visible when the consumer drains on Interrupted.
 *
 * Neither copyable nor movable, like FrameMailbox: it is the shared rendezvous
 * point referred to by both threads, so it has one identity for its lifetime.
 */

#pragma once

#include "RenderRequests.h"

#include <mutex>
#include <vector>

namespace neurus {

/**
 * @brief Mutex-guarded FIFO of RenderRequests, drained a batch at a time.
 */
class RenderRequestQueue
{
public:
	RenderRequestQueue()  = default;
	~RenderRequestQueue() = default;

	RenderRequestQueue(const RenderRequestQueue&)            = delete;
	RenderRequestQueue& operator=(const RenderRequestQueue&) = delete;
	RenderRequestQueue(RenderRequestQueue&&)                 = delete;
	RenderRequestQueue& operator=(RenderRequestQueue&&)      = delete;

	/**
	 * @brief Append one request to the back of the queue.
	 * @param request  The request; moved in, never copied.
	 */
	void Post(RenderRequest&& request);

	/**
	 * @brief Remove and return the whole pending batch in FIFO order.
	 *
	 * Swaps the internal vector out, so a request posted while the returned
	 * batch is being executed lands in the next TakeAll(), not this one.
	 *
	 * @return The requests pending at the moment of the call, oldest first.
	 */
	std::vector<RenderRequest> TakeAll();

	/** @return True when no request is pending. */
	bool Empty() const;

private:
	mutable std::mutex         m_mutex;
	std::vector<RenderRequest> m_requests;
};

} // namespace neurus
