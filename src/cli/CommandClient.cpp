#include "cli/CommandClient.h"

#include "cli/Output.h"
#include "cli/SessionClient.h"

#include <vector>

namespace neurus::cli
{

std::optional<command::CommandResult> RunSessionCommand(
	const std::optional<std::string>& session,
	const command::CommandRequest& request,
	OutputFormat format,
	CliExit& exitCode,
	int timeoutSeconds)
{
	const auto live = LiveSessions();
	std::vector<ipc::SessionRecord> targets;
	std::vector<std::string> ambiguousNames;
	const auto targetName = ResolveSessionNameForCommand(session, live);
	const auto resolution = ResolveTargets(targetName, false, live, targets, ambiguousNames);

	if (resolution == TargetResolution::NoSessions)
	{
		PrintError(format, command::CommandErrorCode::NoSessions,
		           "No running Neurus session was found.");
		exitCode = CliExit::NoSession;
		return std::nullopt;
	}
	if (resolution == TargetResolution::NotFound)
	{
		PrintError(format, command::CommandErrorCode::SessionNotFound,
		           "No running session named " + targetName.value_or("") + ".");
		exitCode = CliExit::NoSession;
		return std::nullopt;
	}
	if (resolution == TargetResolution::Ambiguous)
	{
		PrintError(format, command::CommandErrorCode::AmbiguousSession,
		           "Several sessions are running; pass --session or run 'neurus use <name>'.",
		           {{"sessions", ambiguousNames}});
		exitCode = CliExit::NoSession;
		return std::nullopt;
	}

	command::CommandError clientError{};
	auto result = SendCommand(targets.front(), request, 1, clientError, timeoutSeconds);
	if (!result)
	{
		PrintError(format, clientError.code, clientError.message);
		exitCode = ExitCodeFor(clientError.code);
		return std::nullopt;
	}
	if (!result->ok)
	{
		PrintError(format, result->code, result->message, result->details);
		exitCode = ExitCodeFor(result->code);
		return std::nullopt;
	}

	exitCode = CliExit::Ok;
	return result;
}

} // namespace neurus::cli
