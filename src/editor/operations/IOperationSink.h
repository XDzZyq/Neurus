/**
 * @file IOperationSink.h
 * @brief Interface controllers use to record undoable operations.
 *
 * Controllers depend only on this narrow interface, never on the concrete
 * manager. Submit() is a no-op while replaying (undo/redo re-runs the same
 * handlers synchronously), so handlers stay ignorant of replay state.
 *
 * The script-scope pair lets one script run collapse its writes into a single
 * history entry without any handler knowing a script is running.
 */

#pragma once

#include <memory>
#include <string>

namespace neurus {

class Operation;

/**
 * @brief Sink that records forward operations for undo/redo.
 */
class IOperationSink
{
public:
	virtual ~IOperationSink() = default;

	/**
	 * @brief Records a forward operation describing a just-applied edit.
	 * @param op Operation with absolute before/after values (ownership taken).
	 * @note No-op during replay; recording clears the redo stack.
	 */
	virtual void Submit(std::unique_ptr<Operation> op) = 0;

	/**
	 * @brief Opens the script scope: subsequent Submit() calls buffer into one step.
	 * @param name History label for the run; empty means the default "Run Script".
	 */
	virtual void BeginScriptScope(std::string name = {}) = 0;

	/// @brief Closes the script scope, committing the buffered operations as one step.
	virtual void EndScriptScope() = 0;
};

} // namespace neurus
