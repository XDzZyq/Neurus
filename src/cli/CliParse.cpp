#include "cli/CliParse.h"

#include <system_error>

namespace neurus::cli
{

bool ValidateOpenOptions(const OpenOptions& options, std::string& error)
{
	if (options.timeoutSeconds <= 0)
	{
		error = "--timeout must be positive.";
		return false;
	}
	if (options.name && !ipc::IsValidSessionName(*options.name))
	{
		error = "Invalid session name: " + *options.name;
		return false;
	}
	if (options.project)
	{
		std::error_code code;
		if (!std::filesystem::is_regular_file(*options.project, code))
		{
			error = "Project file not found: " + *options.project;
			return false;
		}
	}
	if (options.app)
	{
		std::error_code code;
		if (!std::filesystem::is_regular_file(*options.app, code))
		{
			error = "Application executable not found: " + options.app->string();
			return false;
		}
	}
	return true;
}

bool ValidateCloseOptions(const CloseOptions& options, std::string& error)
{
	if (options.timeoutSeconds <= 0)
	{
		error = "--timeout must be positive.";
		return false;
	}
	if (options.name && options.all)
	{
		error = "A session name and --all cannot be combined.";
		return false;
	}
	if (options.name && !ipc::IsValidSessionName(*options.name))
	{
		error = "Invalid session name: " + *options.name;
		return false;
	}
	return true;
}

bool ValidateStatusOptions(const StatusOptions& options, std::string& error)
{
	if (options.timeoutSeconds <= 0)
	{
		error = "--timeout must be positive.";
		return false;
	}
	if (options.name && !ipc::IsValidSessionName(*options.name))
	{
		error = "Invalid session name: " + *options.name;
		return false;
	}
	return true;
}

CliExit ExitCodeFor(command::CommandErrorCode code)
{
	using command::CommandErrorCode;
	switch (code)
	{
	case CommandErrorCode::None:
		return CliExit::Ok;
	case CommandErrorCode::AmbiguousSession:
	case CommandErrorCode::NoSessions:
	case CommandErrorCode::SessionNotFound:
		return CliExit::NoSession;
	case CommandErrorCode::Timeout:
	case CommandErrorCode::AppUnreachable:
	case CommandErrorCode::Unauthorized:
	case CommandErrorCode::ProtocolVersionMismatch:
	case CommandErrorCode::FrameInvalid:
	case CommandErrorCode::FrameTooLarge:
	case CommandErrorCode::ServerBusy:
		return CliExit::Transport;
	case CommandErrorCode::DirtyProject:
	case CommandErrorCode::SaveFailed:
	case CommandErrorCode::IoError:
	case CommandErrorCode::ScriptError:
	case CommandErrorCode::ObjectNotFound:
	case CommandErrorCode::AmbiguousObject:
	case CommandErrorCode::Internal:
		return CliExit::Rejected;
	case CommandErrorCode::AppExited:
		return CliExit::AppMissing;
	case CommandErrorCode::InvalidArgument:
	case CommandErrorCode::SessionExists:
		return CliExit::Usage;
	}
	return CliExit::Rejected;
}

TargetResolution ResolveTargets(const std::optional<std::string>& name,
                                bool all,
                                const std::vector<ipc::SessionRecord>& live,
                                std::vector<ipc::SessionRecord>& targets,
                                std::vector<std::string>& ambiguousNames)
{
	targets.clear();
	ambiguousNames.clear();

	if (all)
	{
		if (live.empty())
			return TargetResolution::NoSessions;
		targets = live;
		return TargetResolution::Ok;
	}

	if (name)
	{
		for (const auto& record : live)
		{
			if (record.session == *name)
			{
				targets.push_back(record);
				return TargetResolution::Ok;
			}
		}
		return TargetResolution::NotFound;
	}

	if (live.empty())
		return TargetResolution::NoSessions;
	if (live.size() > 1)
	{
		for (const auto& record : live)
			ambiguousNames.push_back(record.session);
		return TargetResolution::Ambiguous;
	}
	targets.push_back(live.front());
	return TargetResolution::Ok;
}

} // namespace neurus::cli
