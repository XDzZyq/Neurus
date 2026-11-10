#include "cli/subcommands/HistoryCommand.h"

#include "cli/CommandClient.h"
#include "cli/Output.h"
#include "command/Commands.h"

#include <iostream>
#include <string>

namespace neurus::cli
{
namespace
{

void PrintHistory(const command::Json& history, const std::string& action,
                  OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"action", action}, {"history", history}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	for (const char* key : {"undo", "redo"})
	{
		std::cout << key << ":\n";
		for (const auto& label : history[key])
			std::cout << "  " << label.get<std::string>() << '\n';
	}
	std::cout << "can_undo " << (history.value("can_undo", false) ? "yes" : "no")
	          << " can_redo " << (history.value("can_redo", false) ? "yes" : "no") << '\n';
}

} // namespace

void HistoryCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("history", "Report or step the undo/redo history.");
	m_app->require_subcommand(1);

	auto addCommon = [this](CLI::App& app)
	{
		AddSessionOption(app, m_session);
		AddFormatOption(app, m_format);
		app.add_option("--timeout", m_timeoutSeconds, "Command timeout in seconds.")
			->check(CLI::PositiveNumber);
	};

	m_showApp = m_app->add_subcommand("show", "Report the undo/redo stacks.");
	addCommon(*m_showApp);
	m_undoApp = m_app->add_subcommand("undo", "Undo the most recent edit.");
	addCommon(*m_undoApp);
	m_redoApp = m_app->add_subcommand("redo", "Redo the most recently undone edit.");
	addCommon(*m_redoApp);
}

bool HistoryCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit HistoryCommand::Execute()
{
	if (m_showApp != nullptr && m_showApp->parsed())
		return ExecuteAction(command::kHistoryGet, "show");
	if (m_undoApp != nullptr && m_undoApp->parsed())
		return ExecuteAction(command::kHistoryUndo, "undo");
	if (m_redoApp != nullptr && m_redoApp->parsed())
		return ExecuteAction(command::kHistoryRedo, "redo");
	PrintError(OutputFormat::Human, command::CommandErrorCode::InvalidArgument,
	           "A history action is required.");
	return CliExit::Usage;
}

CliExit HistoryCommand::ExecuteAction(const char* command, const char* action)
{
	const OutputFormat format = Format(m_format);
	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_session), {command, {}}, format,
	                                      exitCode, m_timeoutSeconds);
	if (!result)
		return exitCode;
	PrintHistory(result->details, action, format);
	return CliExit::Ok;
}

} // namespace neurus::cli
