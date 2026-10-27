/**
 * @file OperationContext.h
 * @brief Per-replay context passed to Operation::Apply().
 *
 * Operations replay by dispatching their absolute-set event synchronously,
 * carrying integer UIDs; the receiving controller handler resolves them against
 * the current scene and no-ops stale ids. The manager therefore needs no scene
 * or resource access at replay time.
 */

#pragma once

#include "editor/events/IEventQueue.h"

namespace neurus {

/**
 * @brief Context for replaying an operation (event dispatch only).
 */
struct OperationContext
{
	/// @brief Queue the operation replays its event on (via Emit).
	IEventQueue& events;
};

} // namespace neurus
