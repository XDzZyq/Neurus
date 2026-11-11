#include "platform/PlatformProcess.h"

#include "platform/PlatformPaths.h"

#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <vector>

#include <cerrno>
#include <csignal>
#include <crt_externs.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/proc_info.h>
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

std::optional<std::filesystem::path> InstalledAppExecutable()
{
	// A bundle installed in /Applications has no fixed relationship to the
	// CLI's build directory; `open` falls back here after the local lookup.
	const std::filesystem::path candidates[] = {
		"/Applications/Neurus.app/Contents/MacOS/Neurus",
		HomeDirectory() / "Applications" / "Neurus.app" / "Contents" / "MacOS" / "Neurus",
	};
	for (const auto& candidate : candidates)
	{
		if (IsFile(candidate))
			return candidate;
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

	std::vector<std::string> storage;
	storage.push_back(app.string());
	storage.insert(storage.end(), arguments.begin(), arguments.end());
	std::vector<char*> argv;
	argv.reserve(storage.size() + 1);
	for (auto& value : storage)
		argv.push_back(value.data());
	argv.push_back(nullptr);
	const std::string logFile = logPath.string();

	// posix_spawn reports an exec failure synchronously, like CreateProcess on
	// Windows, and its file actions let the child redirect its own descriptors
	// without a fork window that inherits the caller's files.
	posix_spawn_file_actions_t actions;
	if (posix_spawn_file_actions_init(&actions) != 0)
	{
		error = "Cannot prepare the launch file actions.";
		return std::nullopt;
	}
	int action = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
	                                              O_RDONLY, 0);
	if (action == 0)
	{
		action = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logFile.c_str(),
		                                          O_CREAT | O_WRONLY | O_TRUNC, 0600);
	}
	if (action == 0)
		action = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
	if (action != 0)
	{
		posix_spawn_file_actions_destroy(&actions);
		error = "Cannot prepare the launch file actions.";
		return std::nullopt;
	}

	posix_spawnattr_t attributes;
	if (posix_spawnattr_init(&attributes) != 0)
	{
		posix_spawn_file_actions_destroy(&actions);
		error = "Cannot prepare the launch attributes.";
		return std::nullopt;
	}
	// A new session detaches the app from the CLI's terminal, and the default
	// close-on-exec closes every descriptor the CLI happens to hold.
	const short flags = POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT;
	if (posix_spawnattr_setflags(&attributes, flags) != 0)
	{
		posix_spawnattr_destroy(&attributes);
		posix_spawn_file_actions_destroy(&actions);
		error = "Cannot prepare the launch attributes.";
		return std::nullopt;
	}

	pid_t pid = 0;
	char** environment = *_NSGetEnviron();
	const int result = posix_spawn(&pid, app.c_str(), &actions, &attributes, argv.data(),
	                               environment);
	posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);

	if (result != 0)
	{
		error = "Cannot start " + app.string() + ": "
			+ std::generic_category().message(result);
		return std::nullopt;
	}
	return static_cast<int>(pid);
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
	// An exited child stays a zombie until it is reaped, and kill(pid, 0) still
	// succeeds on a zombie, so a launcher would wait forever on an app that died
	// during startup. Reap our own exited child first; for any other pid
	// waitpid fails with ECHILD and the signal probe decides.
	if (::waitpid(static_cast<pid_t>(pid), nullptr, WNOHANG) == static_cast<pid_t>(pid))
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

std::vector<std::string> CommandLineArguments(int argc, char* argv[])
{
	return std::vector<std::string>(argv, argv + argc);
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

	// The POSIX session outlives a `sh -c` wrapper, so a binding survives a
	// script that re-parents the CLI; a terminal always has one session. The
	// leader's start time guards against a reused session id.
	const pid_t sid = ::getsid(0);
	if (sid >= 0)
	{
		proc_bsdinfo info{};
		if (::proc_pidinfo(sid, PROC_PIDTBSDINFO, 0, &info, sizeof(info))
		    == static_cast<int>(sizeof(info)))
		{
			return "session:" + std::to_string(sid) + ":"
				+ std::to_string(info.pbi_start_tvsec) + "."
				+ std::to_string(info.pbi_start_tvusec);
		}
	}

	return "posix:" + std::to_string(::getppid());
}

} // namespace neurus
