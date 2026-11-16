#include "platform/PlatformProcess.h"

#include "platform/PlatformPaths.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace neurus
{
namespace
{

bool IsFile(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

std::optional<unsigned long long> SessionStartTime(pid_t session)
{
	std::ifstream stat("/proc/" + std::to_string(session) + "/stat");
	if (!stat.is_open())
		return std::nullopt;

	std::string line;
	if (!std::getline(stat, line))
		return std::nullopt;

	// The comm field may contain spaces and parentheses, so field counting
	// starts after the last ')'. Field 3 (state) follows it and starttime is
	// field 22, measured in clock ticks since boot.
	const std::size_t commEnd = line.rfind(')');
	if (commEnd == std::string::npos)
		return std::nullopt;

	std::istringstream fields(line.substr(commEnd + 1));
	std::string token;
	for (int field = 3; field < 22; ++field)
	{
		if (!(fields >> token))
			return std::nullopt;
	}

	unsigned long long startTime = 0;
	if (!(fields >> startTime))
		return std::nullopt;
	return startTime;
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

	// The CLI is usually in <build>/<config>/cli, next to the headless app.
	const auto directory = cliPath.parent_path();
	const std::filesystem::path candidates[] = {
		directory / "neurus_headless",
		directory / ".." / "neurus_headless",
	};
	for (const auto& candidate : candidates)
	{
		if (IsFile(candidate))
			return candidate;
	}

	// A PATH install has no fixed relationship to the CLI's build directory.
	const std::string path = EnvironmentValue("PATH");
	std::size_t begin = 0;
	while (begin <= path.size())
	{
		const std::size_t end = path.find(':', begin);
		const auto length = end == std::string::npos ? std::string::npos : end - begin;
		const std::string entry = path.substr(begin, length);
		if (!entry.empty())
		{
			const std::filesystem::path candidate =
				std::filesystem::path(entry) / "neurus_headless";
			if (::access(candidate.c_str(), X_OK) == 0)
				return candidate;
		}
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return std::nullopt;
}

std::optional<std::filesystem::path> InstalledAppExecutable()
{
	const std::filesystem::path candidate =
		HomeDirectory() / ".local" / "bin" / "neurus_headless";
	if (IsFile(candidate))
		return candidate;
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

	// posix_spawn reports an exec failure synchronously and its file actions let
	// the child redirect its own descriptors without a fork window that inherits
	// the caller's files.
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
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
	// glibc has no POSIX_SPAWN_CLOEXEC_DEFAULT; its closefrom action gives the
	// same guarantee, so only stdin, stdout and stderr reach the child.
	if (action == 0)
		action = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
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
	// A new session detaches the app from the CLI's terminal. The libc closes
	// every descriptor the CLI happens to hold either through the spawn flag or
	// the closefrom action above.
	short flags = POSIX_SPAWN_SETSID;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
	flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
	if (posix_spawnattr_setflags(&attributes, flags) != 0)
	{
		posix_spawnattr_destroy(&attributes);
		posix_spawn_file_actions_destroy(&actions);
		error = "Cannot prepare the launch attributes.";
		return std::nullopt;
	}

	pid_t pid = 0;
	const int result = posix_spawn(&pid, app.c_str(), &actions, &attributes, argv.data(),
	                               environ);
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
	// The kernel writes the target without a terminator, and a longer path
	// truncates, so grow the buffer until readlink reports a shorter length.
	std::vector<char> buffer(256);
	for (;;)
	{
		const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
		if (length < 0)
		{
			const int code = errno;
			throw std::runtime_error("Cannot read /proc/self/exe: "
			                         + std::string(std::strerror(code)));
		}
		const auto size = static_cast<std::size_t>(length);
		if (size < buffer.size())
			return std::filesystem::path(std::string(buffer.data(), size));
		buffer.resize(buffer.size() * 2);
	}
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
	int status = 0;
	if (::waitpid(static_cast<pid_t>(pid), &status, WNOHANG) == static_cast<pid_t>(pid))
		return false;
	if (::kill(static_cast<pid_t>(pid), 0) == 0)
		return true;
	return errno == EPERM;
}

std::string EnvironmentValue(const char* name)
{
	const std::size_t length = std::strlen(name);
	for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
	{
		if (std::strncmp(*entry, name, length) == 0 && (*entry)[length] == '=')
			return std::string(*entry + length + 1);
	}
	return {};
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

	// A parent pid changes with every `sh -c` wrapper, so the POSIX session is
	// the stable unit. The leader's start time guards against a reused id.
	const pid_t sid = ::getsid(0);
	if (sid < 0)
		return {};
	const auto startTime = SessionStartTime(sid);
	if (!startTime)
		return {};
	return "session-" + std::to_string(sid) + "-" + std::to_string(*startTime);
}

} // namespace neurus
