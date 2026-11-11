#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neurus
{

/**
 * @brief Finds the Neurus application executable.
 * @param cliPath Path of the running `neurus` executable.
 * @param overridePath Explicit `--app` value, if any.
 * @param envValue `NEURUS_APP` value, if set.
 * @return The executable path, or std::nullopt when none exists.
 */
std::optional<std::filesystem::path> ResolveAppExecutable(
	const std::filesystem::path& cliPath,
	const std::optional<std::filesystem::path>& overridePath,
	const std::string& envValue);

/// @return The path of the running executable.
std::filesystem::path CurrentExecutablePath();

/// @return The `NEURUS_APP` value, or an empty string when unset.
std::string AppEnvironmentPath();

/// @return The calling process id.
int CurrentProcessId();

/// @return True when a process with @p pid is running.
bool IsProcessAlive(int pid);

/// @return The environment value for @p name, or an empty string when unset.
std::string EnvironmentValue(const char* name);

/**
 * @brief Sets or removes an environment variable in this process.
 * @param name Variable name.
 * @param value New value, or nullptr to remove it.
 * @return True on success.
 */
bool SetEnvironmentValue(const char* name, const char* value);

/**
 * @brief Returns the process command line as UTF-8 strings, including argv[0].
 *
 * Windows rebuilds the arguments from the UTF-16 command line so non-ASCII
 * paths and script sources survive a non-UTF-8 ANSI code page. POSIX forwards
 * argv unchanged, where the bytes are already UTF-8.
 *
 * @param argc C runtime argument count.
 * @param argv C runtime argument vector.
 */
std::vector<std::string> CommandLineArguments(int argc, char* argv[]);

/**
 * @brief Returns a stable identifier for the terminal/tab running this process.
 *
 * The CLI uses it to bind a default session per terminal, so parallel agents in
 * separate terminals do not have to pass `--session` on every command.
 *
 * Resolution is: `NEURUS_TERMINAL_ID` override, a terminal-provided session id,
 * otherwise the parent shell process and its creation time. An empty result
 * means the terminal cannot be identified and no binding is stored.
 */
std::string TerminalIdentity();

/**
 * @brief Spawns a detached process with stdout/stderr in a log file.
 * @param app Executable to launch.
 * @param arguments Command-line arguments, excluding the executable.
 * @param logPath File receiving stdout and stderr.
 * @param error Receives a reason when the launch fails.
 * @return The child pid, or std::nullopt with @p error set.
 */
std::optional<int> SpawnDetached(const std::filesystem::path& app,
                                 const std::vector<std::string>& arguments,
                                 const std::filesystem::path& logPath,
                                 std::string& error);

} // namespace neurus
