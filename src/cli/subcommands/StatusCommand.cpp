#include "cli/subcommands/StatusCommand.h"

#include "cli/Output.h"
#include "cli/SessionClient.h"
#include "command/Commands.h"

#include <vector>

namespace neurus::cli
{
namespace
{

CliExit ExecuteStatus(const StatusOptions& options)
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
	const auto targetName = ResolveSessionNameForCommand(options.name, live);
	const auto resolution = ResolveTargets(targetName, false, live, targets, ambiguousNames);

	if (resolution == TargetResolution::NoSessions)
	{
		PrintError(options.format, command::CommandErrorCode::NoSessions,
		           "No running Neurus session was found.");
		return CliExit::NoSession;
	}
	if (resolution == TargetResolution::NotFound)
	{
		PrintError(options.format, command::CommandErrorCode::SessionNotFound,
		           "No running session named " + targetName.value_or("") + ".");
		return CliExit::NoSession;
	}
	if (resolution == TargetResolution::Ambiguous)
	{
		PrintError(options.format, command::CommandErrorCode::AmbiguousSession,
		           "Several sessions are running; pass --session or run 'neurus use <name>'.",
		           {{"sessions", ambiguousNames}});
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

} // namespace

void StatusCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("status", "Report a running session.");
	m_app->add_option("name", m_name, "Session name.");
	AddSessionOption(*m_app, m_session);
	m_app->add_option("--timeout", m_timeoutSeconds, "Command timeout in seconds.")
		->check(CLI::PositiveNumber);
	AddFormatOption(*m_app, m_format);
}

bool StatusCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit StatusCommand::Execute()
{
	StatusOptions options;
	const auto session = SessionName(m_session);
	if (m_name && session)
	{
		PrintError(Format(m_format), command::CommandErrorCode::InvalidArgument,
		           "Pass the session name either positionally or with --session, not both.");
		return CliExit::Usage;
	}
	options.name = m_name ? m_name : session;
	options.timeoutSeconds = m_timeoutSeconds;
	options.format = Format(m_format);
	return ExecuteStatus(options);
}

} // namespace neurus::cli
