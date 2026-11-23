/**
 * @file OperationContext.hpp
 * @brief Per-replay context passed to Operation::Apply().
 *
 * Operations replay by dispatching their absolute-set event synchronously,
 * carrying integer UIDs; the receiving controller handler resolves them against
 * the current scene and no-ops stale ids. The manager therefore needs no scene
 * or resource access at replay time.
 */

#pragma once

#include "editor/events/IEventQueue.hpp"

namespace neurus {

/**
 * @brief Context for replaying an operation (event dispatch only).
 */
class OperationContext
{
	friend class OperationManager;
	friend struct OperationTestAccess;

	explicit OperationContext(IEventQueue& queue, bool replay = false)
		: events(queue), historyReplay(replay) {}

public:
	OperationContext(const OperationContext&) = delete;
	OperationContext& operator=(const OperationContext&) = delete;

	/// @brief Queue the operation replays its event on (via Emit).
	IEventQueue& events;

	/// A committed undo/redo must advance UI revisions even when restoring an older value.
	const bool historyReplay;
};

} // namespace neurus
