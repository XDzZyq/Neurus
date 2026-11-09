#pragma once

#include <cstdint>
#include <string>

namespace neurus::command
{

/**
 * @brief Narrow view of the running application that session commands need.
 *
 * Keeping this interface free of Qt and of Application lets the close policy be
 * tested with a fake and keeps the command core independent of the app layer.
 */
class ISessionState
{
public:
	virtual ~ISessionState() = default;

	/// @return Operating-system process id of the running application.
	virtual int Pid() const = 0;

	/// @return Session name this instance was launched with.
	virtual std::string SessionName() const = 0;

	/// @return Current project path ("" when unsaved).
	virtual std::string ProjectPath() const = 0;

	/// @return True when the document has unsaved changes.
	virtual bool IsDirty() const = 0;

	/**
	 * @brief Persists the current project.
	 * @param error Receives a human-readable reason on failure.
	 * @return True on success.
	 */
	virtual bool Save(std::string& error) = 0;

	/// @return Whole seconds since this session started.
	virtual std::int64_t UptimeSeconds() const = 0;

	/// @return Application version string reported by status.
	virtual std::string AppVersion() const = 0;
};

} // namespace neurus::command
