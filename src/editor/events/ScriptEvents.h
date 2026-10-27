/**
 * @file ScriptEvents.h
 * @brief Run-boundary events that bound one script run to one undo step.
 *
 * A script run is a burst of ordinary scene writes, and each write is recorded
 * by its controller as an individual operation. These two events bracket the run
 * so the operation sink can buffer that burst and commit it as a single history
 * entry - undo/redo then treats the whole run as one edit, which is what "run
 * script" means to a user.
 *
 * The script host emits both synchronously through IEventQueue::Emit, and the
 * ScriptController forwards them to the sink. ScriptRunFinished must be emitted
 * even when the script raises, so a failed run still closes its scope.
 */

#pragma once

namespace neurus {

/** @brief Emitted when a script run begins; opens the one-step undo scope. */
struct ScriptRunStarted {};

/** @brief Emitted when a script run ends; commits the buffered writes as one step. */
struct ScriptRunFinished {};

} // namespace neurus
