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
