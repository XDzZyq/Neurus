#pragma once

#include "command/CommandTypes.h"

#include <string>

namespace neurus::command
{

/**
 * @brief Captures the presented image or every render attachment to disk.
 *
 * Implemented by the application, which owns the render thread and the session
 * name. The command core only parses the request, so it never depends on the
 * renderer.
 */
class ISnapshotCommands
{
public:
	virtual ~ISnapshotCommands() = default;

	/**
	 * @brief Writes a snapshot with the session name injected into @p path.
	 * @param path User destination: a PNG file for a plain capture, or a
	 *             directory for an attachment dump.
	 * @param all True to capture every attachment instead of the swapchain image.
	 * @return Completed result with the final path and written files.
	 */
	virtual CommandResult Capture(const std::string& path, bool all) = 0;
};

} // namespace neurus::command
