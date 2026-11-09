#include "cli/OpenCommand.h"

#include "cli/Output.h"
#include "cli/ProcessLaunch.h"
#include "cli/SessionClient.h"
#include "command/Commands.h"
#include "ipc/SessionRegistry.h"

#include <chrono>
#include <thread>

namespace neurus::cli
{

CliExit RunOpen(const OpenOptions& options)
{
	std::string error;
	if (!ValidateOpenOptions(options, error))
	{
		PrintError(options.format, command::CommandErrorCode::InvalidArgument, error);
		return CliExit::Usage;
	}

	ipc::SessionRegistry registry(ipc::ResolveSessionsDirectory());
	std::string name = options.name
		? *options.name
		: ipc::AllocateSessionName(options.project.value_or(""), registry);
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
	                               BuildAppArguments(name, options.project), logPath, error);
	if (!pid)
	{
		registry.ReleaseClaim(name);
		PrintError(options.format, command::CommandErrorCode::AppExited, error);
		return CliExit::AppMissing;
	}

	if (options.noWait)
	{
		registry.ReleaseClaim(name);
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

} // namespace neurus::cli
