/**
 * @file PublishedFrame.h
 * @brief What one tick hands the render thread: a snapshot and the requests
 *        that belong to it.
 *
 * A snapshot is absolute state and can be made redundant by a newer one; a
 * request is a command and cannot. Keeping them in one value is what lets both
 * rules hold at once: the mailbox may drop an older snapshot, but the requests
 * that arrived with it still travel to the renderer - prepended to the next
 * frame published - so a request executes with the tick that posted it rather
 * than whenever the renderer happens to look at its queue.
 *
 * FIFO order within @c requests is the posting order, and it is load-bearing: an
 * upload posted after DropSceneResourcesRequest must land in the new scene.
 *
 * Vulkan-free and Qt-free, like RenderFrameSnapshot: it is a plain value moved
 * between two threads, never shared.
 */

#pragma once

#include <vector>

#include "render/RenderFrameSnapshot.h"
#include "render/RenderRequests.h"

namespace neurus
{

/**
 * @brief One frame plus the requests posted before it was published.
 */
struct PublishedFrame
{
	/// @brief What to draw. Absolute state: a newer frame makes this one unnecessary.
	RenderFrameSnapshot snapshot;

	/**
	 * @brief Requests posted since the previous publish, FIFO.
	 *
	 * They execute before @c snapshot is drawn, which is what makes a request
	 * that restarts accumulation part of the very frame it was made for.
	 */
	std::vector<RenderRequest> requests;
};

} // namespace neurus
