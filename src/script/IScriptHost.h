#pragma once

#include <string>
#include <vector>

namespace neurus
{

/**
 * @brief Application-level operations a script may invoke.
 *
 * Deliberately excludes new/open: replacing the scene invalidates the active
 * ScriptContext and every proxy in the running script. Project creation and
 * opening stay CLI/UI operations.
 */
class IScriptHost
{
public:
	virtual ~IScriptHost() = default;

	/// @return Current project path ("" when unsaved).
	virtual std::string ProjectPath() const = 0;

	/// @return True when the document has unsaved changes.
	virtual bool IsDirty() const = 0;

	/**
	 * @brief Saves the project to @p path, or to the current path when empty.
	 * @param error Receives a reason on failure.
	 * @return True on success.
	 */
	virtual bool SaveProject(const std::string& path, std::string& error) = 0;

	/**
	 * @brief Captures the viewport or every render attachment.
	 * @param path User destination; the host injects the session name.
	 * @param all True for every attachment.
	 * @param error Receives a reason on failure.
	 * @param files Receives every written file.
	 * @return True on success.
	 */
	virtual bool CaptureSnapshot(const std::string& path, bool all,
	                             std::string& error, std::vector<std::string>& files) = 0;
};

} // namespace neurus
