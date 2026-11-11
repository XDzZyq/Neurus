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

	// Build argv before fork: the child may only make async-signal-safe calls.
	std::vector<std::string> storage;
	storage.push_back(app.string());
	storage.insert(storage.end(), arguments.begin(), arguments.end());
	std::vector<char*> argv;
	argv.reserve(storage.size() + 1);
	for (auto& value : storage)
		argv.push_back(value.data());
	argv.push_back(nullptr);
	const std::string logFile = logPath.string();

	// A close-on-exec pipe reports the launch result: a successful execv closes
	// the write end with nothing written, a failed one writes errno first. This
	// gives the synchronous failure CreateProcess reports on Windows.
	int status[2];
	if (pipe(status) != 0)
	{
		error = "Cannot create the launch status pipe.";
		return std::nullopt;
	}
	fcntl(status[0], F_SETFD, FD_CLOEXEC);
	fcntl(status[1], F_SETFD, FD_CLOEXEC);

	const pid_t pid = fork();
	if (pid < 0)
	{
		close(status[0]);
		close(status[1]);
		error = "Cannot fork the Neurus application.";
		return std::nullopt;
	}
	if (pid == 0)
	{
		close(status[0]);
		setsid();
		const int log = open(logFile.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
		if (log >= 0)
		{
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
			close(log);
		}
		execv(argv[0], argv.data());
		const int code = errno;
		(void)!write(status[1], &code, sizeof(code));
		_exit(127);
	}

	close(status[1]);
	int code = 0;
	ssize_t bytes = 0;
	do
		bytes = read(status[0], &code, sizeof(code));
	while (bytes < 0 && errno == EINTR);
	close(status[0]);

	if (bytes == static_cast<ssize_t>(sizeof(code)))
	{
		waitpid(pid, nullptr, 0);
		error = "Cannot start " + app.string() + ": "
			+ std::generic_category().message(code);
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

	if (const char* tty = ::ttyname(STDIN_FILENO))
		return "tty:" + std::string(tty) + ":" + std::to_string(::getppid());

	return "posix:" + std::to_string(::getppid());
}

} // namespace neurus
