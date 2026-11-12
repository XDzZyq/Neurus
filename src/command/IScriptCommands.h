#pragma once

#include "command/CommandTypes.h"

namespace neurus::command
{

/**
 * @brief Executes Python snippets on the application main thread.
 *
 * Implemented by the application, which owns the embedded interpreter and the
 * script context. The command core only routes by name, so it never depends on
 * the scripting layer.
 */
class IScriptCommands
{
public:
	virtual ~IScriptCommands() = default;

	/**
	 * @brief Executes one script command.
	 * @param request Command request; `script.run` carries `code` or `path`.
	 * @return Completed result with captured output or a script error.
	 */
	virtual CommandResult Execute(const CommandRequest& request) = 0;
};

} // namespace neurus::command
