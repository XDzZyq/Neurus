#include "cli/subcommands/UseCommand.h"

#include "cli/Output.h"
#include "cli/SessionClient.h"
#include "cli/TerminalBinding.h"

#include <iostream>
#include <string>

namespace neurus::cli
{
namespace
{

void PrintBound(const std::string& session, bool live, bool cleared, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		command::Json json{{"ok", true}, {"cleared", cleared}};
		if (cleared)
			json["session"] = nullptr;
		else
			json["session"] = {{"name", session}, {"bound", true}, {"live", live}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	if (cleared)
	{
		std::cout << "cleared terminal session binding\n";
		return;
	}
	std::cout << "session " << session << (live ? " (live)" : " (not running)") << '\n';
}

void PrintNoBinding(OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"session", nullptr}};
		std::cout << json.dump(2) << '\n';
		return;
	}
	std::cout << "no session bound to this terminal\n";
}

} // namespace

void UseCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("use", "Bind a default session to this terminal.");
	m_app->add_option("name", m_name, "Live session to use in this terminal.");
	m_app->add_flag("--clear", m_clear, "Remove this terminal's binding.");
	AddFormatOption(*m_app, m_format);
}

bool UseCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit UseCommand::Execute()
{
	const OutputFormat format = Format(m_format);
	if (m_clear && m_name)
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument,
		           "Pass either a session name or --clear, not both.");
		return CliExit::Usage;
	}

	if (m_clear)
	{
		std::string error;
		if (!ClearTerminalBinding(error))
		{
			PrintError(format, command::CommandErrorCode::IoError, error);
			return CliExit::Rejected;
		}
		PrintBound({}, false, true, format);
		return CliExit::Ok;
	}

	const auto live = LiveSessions();
	if (m_name)
	{
		if (!ipc::IsValidSessionName(*m_name))
		{
			PrintError(format, command::CommandErrorCode::InvalidArgument,
			           "Invalid session name: " + *m_name);
			return CliExit::Usage;
		}
		bool found = false;
		for (const auto& record : live)
		{
			if (record.session == *m_name)
			{
				found = true;
				break;
			}
		}
		if (!found)
		{
			PrintError(format, command::CommandErrorCode::SessionNotFound,
			           "No running session named " + *m_name + ".");
			return CliExit::NoSession;
		}

		std::string error;
		if (!WriteTerminalBinding(*m_name, error))
		{
			PrintError(format, command::CommandErrorCode::IoError, error);
			return CliExit::Rejected;
		}
		PrintBound(*m_name, true, false, format);
		return CliExit::Ok;
	}

	const auto bound = ReadTerminalBinding();
	if (!bound)
	{
		PrintNoBinding(format);
		return CliExit::Ok;
	}

	bool isLive = false;
	for (const auto& record : live)
	{
		if (record.session == *bound)
		{
			isLive = true;
			break;
		}
	}
	PrintBound(*bound, isLive, false, format);
	return CliExit::Ok;
}

} // namespace neurus::cli
