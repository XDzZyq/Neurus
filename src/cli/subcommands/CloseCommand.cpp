#include "cli/subcommands/CloseCommand.h"

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
namespace
{

CliExit ExecuteClose(const CloseOptions& options)
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

} // namespace

void CloseCommand::Register(CLI::App& parent, const std::string& globalFormat)
{
	m_globalFormat = &globalFormat;
	m_app = parent.add_subcommand("close", "Close a running Neurus session.");
	m_app->add_option("name", m_name, "Session name.");
	m_app->add_flag("--all", m_all, "Close every live session.");
	m_app->add_flag("--save", m_save, "Save the project before closing.");
	m_app->add_flag("--force", m_force, "Discard unsaved changes.");
	m_app->add_option("--timeout", m_timeoutSeconds, "Command timeout in seconds.")
		->check(CLI::PositiveNumber);
	AddFormatOption(*m_app, m_format);
}

bool CloseCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit CloseCommand::Execute()
{
	CloseOptions options;
	options.name = m_name;
	options.all = m_all;
	options.save = m_save;
	options.force = m_force;
	options.timeoutSeconds = m_timeoutSeconds;
	options.format = Format(m_format);
	return ExecuteClose(options);
}

} // namespace neurus::cli
