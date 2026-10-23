/**
 * @file RenderRequestQueue.h
 * @brief The renderer's inbox of RenderRequests: a BatchQueue instance.
 *
 * The Editor posts GPU work as values (see RenderRequests.h) and the renderer
 * drains it a whole batch at a time, in strict FIFO order. Those are exactly
 * BatchQueue's guarantees, so this is just that template bound to RenderRequest
 * rather than a second queue to keep in step with it. The whole-batch swap-out
 * is the ordering contract RenderRequests.h relies on: an upload posted after
 * DropSceneResourcesRequest lands in the new scene, never mid-iteration over
 * the batch that dropped it.
 */

#pragma once

#include "core/BatchQueue.h"
#include "RenderRequests.h"

namespace neurus {

/// Thread-safe FIFO of RenderRequests, drained one batch per frame.
using RenderRequestQueue = BatchQueue<RenderRequest>;

} // namespace neurus
