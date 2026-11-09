#pragma once

#include "command/CommandTypes.h"

namespace neurus::command
{

/**
 * @brief Executes one command on the application main thread.
 *
 * Implementations must never be called from the IPC thread.
 */
class ICommandExecutor
{
public:
	virtual ~ICommandExecutor() = default;

	/**
	 * @brief Executes a command and returns its completed result.
	 * @param request Validated command request.
	 * @return Structured result; never throws for an unknown command.
	 */
	virtual CommandResult Execute(const CommandRequest& request) = 0;
};

} // namespace neurus::command
