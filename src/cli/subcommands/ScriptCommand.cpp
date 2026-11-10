#include "cli/subcommands/ScriptCommand.h"

#include "cli/CommandClient.h"
#include "cli/Output.h"
#include "command/Commands.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace neurus::cli
{
namespace
{

void PrintScriptOutput(const command::Json& result, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"script", result}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	const std::string output = result.value("output", std::string{});
	std::cout << output;
	if (!output.empty() && output.back() != '\n')
		std::cout << '\n';
}

} // namespace

void ScriptCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("script", "Run Python in the application.");
	m_app->require_subcommand(1);

	m_runApp = m_app->add_subcommand("run", "Run Python code or a script file.");
	m_runApp->add_option("--code", m_code, "Python source to execute.");
	m_runApp->add_option("-f,--file", m_file, "Python file to execute.");
	AddSessionOption(*m_runApp, m_session);
	AddFormatOption(*m_runApp, m_format);
	m_runApp->add_option("--timeout", m_timeoutSeconds, "Command timeout in seconds.")
		->check(CLI::PositiveNumber);
}

bool ScriptCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit ScriptCommand::Execute()
{
	if (m_runApp == nullptr || !m_runApp->parsed())
	{
		PrintError(OutputFormat::Human, command::CommandErrorCode::InvalidArgument,
		           "A script action is required.");
		return CliExit::Usage;
	}

	const OutputFormat format = Format(m_format);
	if (m_code.empty() == m_file.empty())
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument,
		           "Pass exactly one of --code or --file.");
		return CliExit::Usage;
	}

	command::Json args;
	if (!m_file.empty())
	{
		std::error_code pathError;
		const auto absolute = std::filesystem::absolute(m_file, pathError);
		const std::string path = pathError ? m_file : absolute.string();
		std::error_code fileError;
		if (!std::filesystem::is_regular_file(path, fileError))
		{
			PrintError(format, command::CommandErrorCode::IoError,
			           "Script file not found: " + path);
			return CliExit::Rejected;
		}
		args["path"] = path;
	}
	else
	{
		args["code"] = m_code;
	}

	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_session),
	                                      {command::kScriptRun, args}, format, exitCode,
	                                      m_timeoutSeconds);
	if (!result)
		return exitCode;

	PrintScriptOutput(result->details, format);
	return CliExit::Ok;
}

} // namespace neurus::cli
