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

bool ReadRequiredString(const command::Json& arguments, const char* key,
                        std::string& value, std::string& error)
{
	if (!arguments.contains(key) || !arguments[key].is_string())
	{
		error = std::string("'") + key + "' must be a string.";
		return false;
	}
	value = arguments[key].get<std::string>();
	if (value.empty())
	{
		error = std::string("'") + key + "' cannot be empty.";
		return false;
	}
	return true;
}

bool ReadOptionalString(const command::Json& arguments, const char* key,
                        std::string& value, bool& present, std::string& error)
{
	present = arguments.contains(key);
	if (!present)
		return true;
	if (!arguments[key].is_string())
	{
		error = std::string("'") + key + "' must be a string.";
		return false;
	}
	value = arguments[key].get<std::string>();
	return true;
}

} // namespace

SessionCommandExecutor::SessionCommandExecutor(command::ISessionState& state,
                                               std::function<void()> requestClose,
                                               command::IEditorCommands* editor)
	: m_state(state)
	, m_requestClose(std::move(requestClose))
	, m_editor(editor)
{
}

command::CommandResult SessionCommandExecutor::Execute(const command::CommandRequest& request)
{
	if (request.name == command::kSessionStatus)
		return ExecuteStatus();
	if (request.name == command::kSessionClose)
		return ExecuteClose(request);

	if (request.name.rfind("project.", 0) == 0)
		return ExecuteProject(request);

	if (command::IsEditorCommand(request.name) && m_editor != nullptr)
		return m_editor->Execute(request);

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
		{"objects", m_state.SceneObjectCount()},
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

command::CommandResult SessionCommandExecutor::ExecuteProject(const command::CommandRequest& request)
{
	if (request.name == command::kProjectInfo)
		return ExecuteProjectInfo();
	if (request.name == command::kProjectNew)
		return ExecuteProjectNew(request);
	if (request.name == command::kProjectOpen)
		return ExecuteProjectOpen(request);
	if (request.name == command::kProjectSave)
		return ExecuteProjectSave(request);

	return command::CommandResult::Failure(command::CommandErrorCode::InvalidArgument,
	                                      "Unknown command: " + request.name);
}

command::CommandResult SessionCommandExecutor::ExecuteProjectInfo() const
{
	return command::CommandResult::Success({
		{"session", m_state.SessionName()},
		{"path", m_state.ProjectPath()},
		{"dirty", m_state.IsDirty()},
		{"objects", m_state.SceneObjectCount()},
		{"app_version", m_state.AppVersion()},
		{"protocol_version", command::kProtocolVersion},
		{"uptime_s", m_state.UptimeSeconds()},
	});
}

command::CommandResult SessionCommandExecutor::ExecuteProjectNew(const command::CommandRequest& request)
{
	bool force = false;
	if (!ReadOptionalBool(request.args, "force", false, force))
	{
		return command::CommandResult::Failure(command::CommandErrorCode::InvalidArgument,
		                                      "'force' must be a boolean.");
	}

	if (m_state.IsDirty() && !force)
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::DirtyProject,
			"The project has unsaved changes; pass force to discard them.",
			{{"project", m_state.ProjectPath()}});
	}

	std::string error;
	if (!m_state.NewProject(force, error))
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::IoError,
			error.empty() ? "Creating a new project failed." : error);
	}

	return command::CommandResult::Success({
		{"path", m_state.ProjectPath()},
		{"dirty", m_state.IsDirty()},
		{"objects", m_state.SceneObjectCount()},
	});
}

command::CommandResult SessionCommandExecutor::ExecuteProjectOpen(const command::CommandRequest& request)
{
	std::string path;
	std::string error;
	if (!ReadRequiredString(request.args, "path", path, error))
	{
		return command::CommandResult::Failure(command::CommandErrorCode::InvalidArgument, error);
	}

	bool force = false;
	if (!ReadOptionalBool(request.args, "force", false, force))
	{
		return command::CommandResult::Failure(command::CommandErrorCode::InvalidArgument,
		                                      "'force' must be a boolean.");
	}

	if (m_state.IsDirty() && !force)
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::DirtyProject,
			"The project has unsaved changes; pass force to discard them.",
			{{"project", m_state.ProjectPath()}, {"open", path}});
	}

	if (!m_state.OpenProject(path, force, error))
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::IoError,
			error.empty() ? "Opening the project failed." : error,
			{{"path", path}});
	}

	return command::CommandResult::Success({
		{"path", m_state.ProjectPath()},
		{"dirty", m_state.IsDirty()},
		{"objects", m_state.SceneObjectCount()},
	});
}

command::CommandResult SessionCommandExecutor::ExecuteProjectSave(const command::CommandRequest& request)
{
	std::string path;
	bool hasPath = false;
	std::string error;
	if (!ReadOptionalString(request.args, "path", path, hasPath, error))
	{
		return command::CommandResult::Failure(command::CommandErrorCode::InvalidArgument, error);
	}

	const bool saved = hasPath && !path.empty()
		? m_state.SaveAs(path, error)
		: m_state.Save(error);
	if (!saved)
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::SaveFailed,
			error.empty() ? "Saving the project failed." : error,
			{{"path", hasPath ? path : m_state.ProjectPath()}});
	}

	return command::CommandResult::Success({
		{"path", m_state.ProjectPath()},
		{"dirty", m_state.IsDirty()},
		{"objects", m_state.SceneObjectCount()},
		{"saved", true},
	});
}

} // namespace neurus
