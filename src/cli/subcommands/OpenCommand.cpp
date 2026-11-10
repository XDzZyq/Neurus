#include "cli/subcommands/OpenCommand.h"

#include "cli/AppArguments.h"
#include "cli/Output.h"
#include "cli/SessionClient.h"
#include "cli/TerminalBinding.h"
#include "command/Commands.h"
#include "ipc/SessionRegistry.h"
#include "platform/PlatformProcess.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

namespace neurus::cli
{
namespace
{

void PrintStarting(const std::string& session, int pid, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{
			{"ok", true},
			{"session", {{"name", session}, {"pid", pid}, {"starting", true}}},
		};
		std::cout << json.dump(2) << '\n';
		return;
	}
	std::cout << "session " << session << " pid " << pid << " (starting)\n";
}

std::string ReadLogTail(const std::filesystem::path& path, std::size_t maxLines)
{
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open())
		return {};

	std::vector<std::string> lines;
	std::string line;
	while (std::getline(in, line))
	{
		lines.push_back(std::move(line));
		if (lines.size() > maxLines * 2)
			lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(maxLines));
	}

	std::ostringstream out;
	const std::size_t first = lines.size() > maxLines ? lines.size() - maxLines : 0;
	for (std::size_t i = first; i < lines.size(); ++i)
		out << lines[i] << '\n';
	return out.str();
}

CliExit ExecuteOpen(const OpenOptions& options)
{
	OpenOptions resolved = options;
	if (resolved.project)
	{
		std::error_code pathError;
		const auto absolute = std::filesystem::absolute(*resolved.project, pathError);
		if (!pathError)
			resolved.project = absolute.string();
	}

	std::string error;
	if (!ValidateOpenOptions(resolved, error))
	{
		PrintError(options.format, command::CommandErrorCode::InvalidArgument, error);
		return CliExit::Usage;
	}

	ipc::SessionRegistry registry(ipc::ResolveSessionsDirectory());
	std::string name = options.name
		? *options.name
		: ipc::AllocateSessionName(resolved.project.value_or(""), registry);
	if (!ipc::IsValidSessionName(name))
	{
		PrintError(options.format, command::CommandErrorCode::InvalidArgument,
		           "Cannot allocate a valid session name.");
		return CliExit::Usage;
	}

	if (!registry.TryClaim(name, error))
	{
		PrintError(options.format, command::CommandErrorCode::SessionExists, error);
		return CliExit::Usage;
	}

	const auto resolvedApp = ResolveAppExecutable(
		CurrentExecutablePath(), options.app, AppEnvironmentPath());
	if (!resolvedApp)
	{
		registry.ReleaseClaim(name);
		PrintError(options.format, command::CommandErrorCode::AppExited,
		           "Cannot find the Neurus executable; pass --app or set NEURUS_APP.");
		return CliExit::AppMissing;
	}

	const auto logPath = registry.Directory() / (name + ".log");
	const auto pid = SpawnDetached(*resolvedApp,
	                               BuildAppArguments(name, resolved.project), logPath, error);
	if (!pid)
	{
		registry.ReleaseClaim(name);
		PrintError(options.format, command::CommandErrorCode::AppExited, error);
		return CliExit::AppMissing;
	}

	if (options.noWait)
	{
		registry.ReleaseClaim(name);
		if (!options.noBind)
		{
			std::string bindError;
			if (!WriteTerminalBinding(name, bindError))
				std::cerr << "warning: " << bindError << '\n';
		}
		PrintStarting(name, *pid, options.format);
		return CliExit::Ok;
	}

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::seconds(options.timeoutSeconds);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (const auto record = registry.Read(name))
		{
			command::CommandError clientError{};
			const auto result = SendCommand(*record, {command::kSessionStatus, {}}, 1,
			                                clientError, 1);
			if (result && result->ok)
			{
				registry.ReleaseClaim(name);
				if (!options.noBind)
				{
					std::string bindError;
					if (!WriteTerminalBinding(name, bindError))
						std::cerr << "warning: " << bindError << '\n';
				}
				PrintSession(*record, result->details.value("dirty", false), options.format);
				return CliExit::Ok;
			}
		}

		if (!ipc::IsProcessAlive(*pid))
		{
			registry.ReleaseClaim(name);
			registry.Remove(name);
			std::string message = "Neurus exited before the session became ready.";
			const auto tail = ReadLogTail(logPath, 20);
			if (!tail.empty())
				message += "\n" + tail;
			PrintError(options.format, command::CommandErrorCode::AppExited, message);
			return CliExit::AppMissing;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	registry.ReleaseClaim(name);
	PrintError(options.format, command::CommandErrorCode::Timeout,
	           "Timed out waiting for the session to become ready.");
	return CliExit::Transport;
}

} // namespace

void OpenCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("open", "Launch a named background Neurus session.");
	m_app->add_option("project", m_project, "Project file to open.");
	m_app->add_option("--name", m_name, "Session name; allocated when omitted.");
	m_app->add_option("--app", m_appPath, "Path to the Neurus executable.");
	m_app->add_option("--timeout", m_timeoutSeconds, "Readiness timeout in seconds.")
		->check(CLI::PositiveNumber);
	m_app->add_flag("--no-wait", m_noWait, "Do not wait for readiness.");
	m_app->add_flag("--no-bind", m_noBind, "Do not bind the session to this terminal.");
	AddFormatOption(*m_app, m_format);
}

bool OpenCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit OpenCommand::Execute()
{
	OpenOptions options;
	options.project = m_project;
	options.name = m_name;
	if (m_appPath)
		options.app = std::filesystem::path(*m_appPath);
	options.timeoutSeconds = m_timeoutSeconds;
	options.noWait = m_noWait;
	options.noBind = m_noBind;
	options.format = Format(m_format);
	return ExecuteOpen(options);
}

} // namespace neurus::cli
