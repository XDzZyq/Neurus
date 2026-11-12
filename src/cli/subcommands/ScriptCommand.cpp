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

	const std::string error = result.value("stderr", std::string{});
	if (!error.empty())
	{
		std::cerr << error;
		if (error.back() != '\n')
			std::cerr << '\n';
	}
}

void PrintScriptEval(const command::Json& result, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"script", result}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	const std::string output = result.value("output", std::string{});
	if (!output.empty())
	{
		std::cout << output;
		if (output.back() != '\n')
			std::cout << '\n';
	}

	const std::string error = result.value("stderr", std::string{});
	if (!error.empty())
	{
		std::cerr << error;
		if (error.back() != '\n')
			std::cerr << '\n';
	}

	std::cout << result.value("result", std::string{}) << '\n';
}

std::string JoinStrings(const command::Json& array)
{
	std::string joined;
	for (const auto& value : array)
	{
		if (!joined.empty())
			joined += ' ';
		joined += value.get<std::string>();
	}
	return joined;
}

void PrintScriptInfo(const command::Json& info, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"script", info}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	std::cout << "ready " << (info.value("ready", false) ? "yes" : "no") << '\n';
	std::cout << "python_version " << info.value("python_version", std::string{}) << '\n';
	std::cout << "environment " << info.value("environment", std::string{}) << '\n';
	std::cout << "console_lines " << info.value("console_lines", 0) << '\n';
	std::cout << "modules " << info.value("module_count", 0) << '\n';
	if (info.contains("modules"))
	{
		for (const auto& module : info["modules"])
		{
			std::cout << "  " << module.value("name", std::string{}) << ' '
			          << module.value("origin", std::string{}) << ' '
			          << module.value("seconds", 0.0) << "s";
			if (module.value("project", false))
				std::cout << " project";
			if (module.value("cached", false))
				std::cout << " cached";
			std::cout << '\n';
		}
	}
	if (info.value("modules_truncated", false))
		std::cout << "modules_truncated yes\n";

	const command::Json sandbox = info.value("sandbox", command::Json::object());
	const std::string readRoots = JoinStrings(sandbox.value("read_roots", command::Json::array()));
	const std::string writeRoots = JoinStrings(sandbox.value("write_roots", command::Json::array()));
	const std::string blocked = JoinStrings(sandbox.value("blocked_modules", command::Json::array()));
	std::cout << "sandbox.read_roots " << (readRoots.empty() ? "(none)" : readRoots) << '\n';
	std::cout << "sandbox.write_roots " << (writeRoots.empty() ? "(none)" : writeRoots) << '\n';
	std::cout << "sandbox.blocked_modules " << (blocked.empty() ? "(none)" : blocked) << '\n';
	std::cout << "sandbox.network " << (sandbox.value("network", true) ? "yes" : "no") << '\n';
	std::cout << "sandbox.subprocess " << (sandbox.value("subprocess", true) ? "yes" : "no") << '\n';
	std::cout << "sandbox.ctypes " << (sandbox.value("ctypes", true) ? "yes" : "no") << '\n';
	std::cout << "sandbox.time_limit_s " << sandbox.value("time_limit_s", 0.0) << '\n';
	std::cout << "sandbox.memory_limit_mb " << sandbox.value("memory_limit_mb", 0) << '\n';
}

void PrintScriptConsole(const command::Json& result, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"script", result}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	for (const auto& line : result["lines"])
	{
		const std::string text = line.value("text", std::string{});
		std::cout << text;
		if (!text.empty() && text.back() != '\n')
			std::cout << '\n';
	}
}

void PrintScriptClear(OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"script", {{"cleared", true}}}};
		std::cout << json.dump(2) << '\n';
		return;
	}
	std::cout << "script console cleared\n";
}

} // namespace

void ScriptCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("script", "Run Python in the application.");
	m_app->require_subcommand(1);

	auto addCommon = [this](CLI::App& app)
	{
		AddSessionOption(app, m_session);
		AddFormatOption(app, m_format);
		app.add_option("--timeout", m_timeoutSeconds,
		               "Command timeout in seconds (default 300 to absorb a cold import).")
			->check(CLI::PositiveNumber);
	};

	m_runApp = m_app->add_subcommand("run", "Run Python code or a script file.");
	m_runApp->add_option("--code", m_code, "Python source to execute.");
	m_runApp->add_option("-f,--file", m_file, "Python file to execute.");
	addCommon(*m_runApp);

	m_infoApp = m_app->add_subcommand("info", "Report scripting readiness and versions.");
	addCommon(*m_infoApp);

	m_evalApp = m_app->add_subcommand("eval", "Evaluate a Python expression.");
	m_evalApp->add_option("code", m_evalCode, "Python expression to evaluate.")->required();
	addCommon(*m_evalApp);

	m_consoleApp = m_app->add_subcommand("console", "Read captured script console lines.");
	m_consoleApp->add_option("--tail", m_tail, "Number of trailing lines.")
		->check(CLI::PositiveNumber);
	addCommon(*m_consoleApp);

	m_clearApp = m_app->add_subcommand("clear", "Clear the script console.");
	addCommon(*m_clearApp);
}

bool ScriptCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit ScriptCommand::Execute()
{
	const OutputFormat format = Format(m_format);
	CliExit exitCode = CliExit::Ok;

	if (m_infoApp != nullptr && m_infoApp->parsed())
	{
		const auto result = RunSessionCommand(SessionName(m_session),
		                                      {command::kScriptInfo, {}}, format, exitCode,
		                                      m_timeoutSeconds);
		if (!result)
			return exitCode;
		PrintScriptInfo(result->details, format);
		return CliExit::Ok;
	}

	if (m_consoleApp != nullptr && m_consoleApp->parsed())
	{
		const auto result = RunSessionCommand(
			SessionName(m_session), {command::kScriptConsole, {{"tail", m_tail}}}, format,
			exitCode, m_timeoutSeconds);
		if (!result)
			return exitCode;
		PrintScriptConsole(result->details, format);
		return CliExit::Ok;
	}

	if (m_clearApp != nullptr && m_clearApp->parsed())
	{
		const auto result = RunSessionCommand(SessionName(m_session),
		                                      {command::kScriptClear, {}}, format, exitCode,
		                                      m_timeoutSeconds);
		if (!result)
			return exitCode;
		PrintScriptClear(format);
		return CliExit::Ok;
	}

	if (m_evalApp != nullptr && m_evalApp->parsed())
	{
		command::Json args{{"code", m_evalCode}};
		const auto result = RunSessionCommand(SessionName(m_session),
		                                      {command::kScriptEval, args}, format, exitCode,
		                                      m_timeoutSeconds);
		if (!result)
			return exitCode;
		PrintScriptEval(result->details, format);
		return CliExit::Ok;
	}

	if (m_runApp == nullptr || !m_runApp->parsed())
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument,
		           "A script action is required.");
		return CliExit::Usage;
	}

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

	const auto result = RunSessionCommand(SessionName(m_session),
	                                      {command::kScriptRun, args}, format, exitCode,
	                                      m_timeoutSeconds);
	if (!result)
		return exitCode;

	PrintScriptOutput(result->details, format);
	return CliExit::Ok;
}

} // namespace neurus::cli
