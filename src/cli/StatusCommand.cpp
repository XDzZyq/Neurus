#include "cli/StatusCommand.h"

#include "cli/Output.h"
#include "cli/SessionClient.h"
#include "command/Commands.h"

#include <vector>

namespace neurus::cli
{

CliExit RunStatus(const StatusOptions& options)
{
	std::string error;
	if (!ValidateStatusOptions(options, error))
	{
		PrintError(options.format, command::CommandErrorCode::InvalidArgument, error);
		return CliExit::Usage;
	}

	const auto live = LiveSessions();
	std::vector<ipc::SessionRecord> targets;
	std::vector<std::string> ambiguousNames;
	const auto resolution = ResolveTargets(options.name, false, live, targets, ambiguousNames);

	if (resolution == TargetResolution::NoSessions)
	{
		PrintError(options.format, command::CommandErrorCode::NoSessions,
		           "No running Neurus session was found.");
		return CliExit::NoSession;
	}
	if (resolution == TargetResolution::NotFound)
	{
		PrintError(options.format, command::CommandErrorCode::SessionNotFound,
		           "No running session named " + options.name.value_or("") + ".");
		return CliExit::NoSession;
	}
	if (resolution == TargetResolution::Ambiguous)
	{
		PrintError(options.format, command::CommandErrorCode::AmbiguousSession,
		           "Several sessions are running; name one.", {{"sessions", ambiguousNames}});
		return CliExit::NoSession;
	}

	const auto& record = targets.front();
	command::CommandError clientError{};
	auto result = SendCommand(record, {command::kSessionStatus, {}}, 1, clientError,
	                          options.timeoutSeconds);
	if (!result)
	{
		PrintError(options.format, clientError.code, clientError.message);
		return ExitCodeFor(clientError.code);
	}
	if (!result->ok)
	{
		PrintError(options.format, result->code, result->message, result->details);
		return ExitCodeFor(result->code);
	}

	PrintSession(record, result->details.value("dirty", false), options.format);
	return CliExit::Ok;
}

} // namespace neurus::cli
