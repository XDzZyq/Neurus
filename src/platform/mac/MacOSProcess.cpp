#include "platform/PlatformProcess.h"

#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <vector>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace neurus
{
namespace
{

bool IsFile(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

} // namespace

std::optional<std::filesystem::path> ResolveAppExecutable(
	const std::filesystem::path& cliPath,
	const std::optional<std::filesystem::path>& overridePath,
	const std::string& envValue)
{
	if (overridePath && IsFile(*overridePath))
		return *overridePath;
	if (!envValue.empty() && IsFile(envValue))
		return std::filesystem::path(envValue);

	const std::filesystem::path base = cliPath.empty()
		? std::filesystem::current_path()
		: std::filesystem::absolute(cliPath);
	const auto directory = base.parent_path();
	const auto parent = directory.parent_path();
	const auto grandparent = parent.parent_path();

	// The CLI is usually in <build>/<config>/cli, next to the app. A macOS
	// build is a bundle, so the executable is inside Neurus.app/Contents/MacOS.
	const std::filesystem::path names[] = {
		"Neurus.app/Contents/MacOS/Neurus",
		"Neurus",
	};

	for (const auto& name : names)
	{
		for (const auto& candidate : {parent / name, directory / name, grandparent / name})
		{
			if (IsFile(candidate))
				return candidate;
		}
	}
	return std::nullopt;
}

std::optional<int> SpawnDetached(const std::filesystem::path& app,
                                 const std::vector<std::string>& arguments,
                                 const std::filesystem::path& logPath,
                                 std::string& error)
{
	std::error_code directoryError;
	std::filesystem::create_directories(logPath.parent_path(), directoryError);

	const int pid = fork();
	if (pid < 0)
	{
		error = "Cannot fork the Neurus application.";
		return std::nullopt;
	}
	if (pid == 0)
	{
		setsid();
		const int log = open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
		if (log >= 0)
		{
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
			close(log);
		}

		std::vector<std::string> storage;
		storage.push_back(app.string());
		storage.insert(storage.end(), arguments.begin(), arguments.end());
		std::vector<char*> argv;
		argv.reserve(storage.size() + 1);
		for (auto& value : storage)
			argv.push_back(value.data());
		argv.push_back(nullptr);
		execv(app.c_str(), argv.data());
		_exit(127);
	}
	return pid;
}

std::filesystem::path CurrentExecutablePath()
{
	std::uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> buffer(size);
	if (_NSGetExecutablePath(buffer.data(), &size) != 0)
		return {};
	std::error_code error;
	const auto resolved = std::filesystem::weakly_canonical(buffer.data(), error);
	return error ? std::filesystem::path(buffer.data()) : resolved;
}

std::string AppEnvironmentPath()
{
	return EnvironmentValue("NEURUS_APP");
}

int CurrentProcessId()
{
	return static_cast<int>(::getpid());
}

bool IsProcessAlive(int pid)
{
	if (pid <= 0)
		return false;
	if (::kill(static_cast<pid_t>(pid), 0) == 0)
		return true;
	return errno == EPERM;
}

std::string EnvironmentValue(const char* name)
{
	const char* value = std::getenv(name);
	return value ? std::string(value) : std::string{};
}

bool SetEnvironmentValue(const char* name, const char* value)
{
	if (value == nullptr)
		return ::unsetenv(name) == 0;
	return ::setenv(name, value, 1) == 0;
}

std::string TerminalIdentity()
{
	const std::string override = EnvironmentValue("NEURUS_TERMINAL_ID");
	if (!override.empty())
		return "explicit:" + override;

	for (const char* name : {"TERM_SESSION_ID", "TMUX_PANE"})
	{
		const std::string value = EnvironmentValue(name);
		if (!value.empty())
			return std::string(name) + ":" + value;
	}

	if (const char* tty = ::ttyname(STDIN_FILENO))
		return "tty:" + std::string(tty) + ":" + std::to_string(::getppid());

	return "posix:" + std::to_string(::getppid());
}

} // namespace neurus
