#include "cli/CloseCommand.h"

#include "cli/Output.h"
#include "cli/SessionClient.h"
#include "command/CommandError.h"
#include "command/Commands.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace neurus::cli
{

CliExit RunClose(const CloseOptions& options)
{
	std::string error;
	if (!ValidateCloseOptions(options, error))
	{
		PrintError(options.format, command::CommandErrorCode::InvalidArgument, error);
		return CliExit::Usage;
	}

	const auto live = LiveSessions();
	std::vector<ipc::SessionRecord> targets;
	std::vector<std::string> ambiguousNames;
	const auto resolution = ResolveTargets(options.name, options.all, live, targets, ambiguousNames);

	switch (resolution)
	{
	case TargetResolution::NoSessions:
		PrintError(options.format, command::CommandErrorCode::NoSessions,
		           "No running Neurus session was found.");
		return CliExit::NoSession;
	case TargetResolution::NotFound:
		PrintError(options.format, command::CommandErrorCode::SessionNotFound,
		           "No running session named " + options.name.value_or("") + ".");
		return CliExit::NoSession;
	case TargetResolution::Ambiguous:
		PrintError(options.format, command::CommandErrorCode::AmbiguousSession,
		           "Several sessions are running; name one or pass --all.", {{"sessions", ambiguousNames}});
		return CliExit::NoSession;
	case TargetResolution::Ok:
		break;
	}

	const int timeout = options.save ? std::max(options.timeoutSeconds, 60) : options.timeoutSeconds;
	const command::Json args{{"save", options.save}, {"force", options.force}};

	std::vector<std::pair<std::string, command::CommandResult>> results;
	std::uint64_t id = 1;
	for (const auto& record : targets)
	{
		command::CommandError clientError{};
		auto result = SendCommand(record, {command::kSessionClose, args}, id++, clientError, timeout);
		if (!result)
			result = command::CommandResult::Failure(clientError.code, clientError.message);
		results.emplace_back(record.session, *result);
	}

	PrintCloseResults(results, options.format);

	for (const auto& [session, result] : results)
	{
		if (!result.ok)
			return ExitCodeFor(result.code);
	}
	return CliExit::Ok;
}

} // namespace neurus::cli
