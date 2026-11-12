#pragma once

#include "command/CommandTypes.h"

namespace neurus::command
{

/**
 * @brief Executes scene and object commands on the application main thread.
 *
 * Implemented by the editor layer, which owns the scene, the resource pool and
 * the typed event queue. The command core only routes by name, so it never
 * depends on scene types.
 */
class IEditorCommands
{
public:
	virtual ~IEditorCommands() = default;

	/**
	 * @brief Executes one scene or object command.
	 * @param request Command request with command-specific arguments.
	 * @return Completed result; never throws for an unknown command.
	 */
	virtual CommandResult Execute(const CommandRequest& request) = 0;
};

} // namespace neurus::command
