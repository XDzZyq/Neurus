#include "app/session/SessionCommandExecutor.h"

#include "command/CommandJson.h"
#include "command/Commands.h"

#include <string>
#include <utility>

namespace neurus
{
namespace
{

bool ReadOptionalBool(const command::Json& arguments, const char* key, bool fallback, bool& value)
{
	if (!arguments.contains(key))
	{
		value = fallback;
		return true;
	}
	if (!arguments[key].is_boolean())
		return false;
	value = arguments[key].get<bool>();
	return true;
}

} // namespace

SessionCommandExecutor::SessionCommandExecutor(command::ISessionState& state,
                                               std::function<void()> requestClose)
	: m_state(state)
	, m_requestClose(std::move(requestClose))
{
}

command::CommandResult SessionCommandExecutor::Execute(const command::CommandRequest& request)
{
	if (request.name == command::kSessionStatus)
		return ExecuteStatus();
	if (request.name == command::kSessionClose)
		return ExecuteClose(request);

	return command::CommandResult::Failure(command::CommandErrorCode::InvalidArgument,
	                                      "Unknown command: " + request.name);
}

bool SessionCommandExecutor::CloseRequested() const
{
	return m_closeRequested;
}

command::CommandResult SessionCommandExecutor::ExecuteStatus() const
{
	return command::CommandResult::Success({
		{"session", m_state.SessionName()},
		{"pid", m_state.Pid()},
		{"project", m_state.ProjectPath()},
		{"dirty", m_state.IsDirty()},
		{"app_version", m_state.AppVersion()},
		{"protocol_version", command::kProtocolVersion},
		{"uptime_s", m_state.UptimeSeconds()},
	});
}

command::CommandResult SessionCommandExecutor::ExecuteClose(const command::CommandRequest& request)
{
	bool save = false;
	bool force = false;
	if (!ReadOptionalBool(request.args, "save", false, save)
	    || !ReadOptionalBool(request.args, "force", false, force))
	{
		return command::CommandResult::Failure(command::CommandErrorCode::InvalidArgument,
		                                      "'save' and 'force' must be booleans.");
	}

	const bool dirty = m_state.IsDirty();
	if (dirty && !force && !save)
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::DirtyProject,
			"The project has unsaved changes; pass save or force.",
			{{"session", m_state.SessionName()}, {"project", m_state.ProjectPath()}});
	}

	if (dirty && save && !force)
	{
		std::string error;
		if (!m_state.Save(error))
		{
			return command::CommandResult::Failure(command::CommandErrorCode::SaveFailed,
			                                      error.empty() ? "Saving the project failed."
			                                                    : error);
		}
	}

	m_closeRequested = true;
	if (m_requestClose)
		m_requestClose();

	return command::CommandResult::Success({
		{"session", m_state.SessionName()},
		{"pid", m_state.Pid()},
	});
}

} // namespace neurus
