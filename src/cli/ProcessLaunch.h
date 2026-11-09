#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neurus::cli
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

/// @return The path of the running `neurus` executable.
std::filesystem::path CurrentExecutablePath();

/// @return The `NEURUS_APP` value, or an empty string when unset.
std::string AppEnvironmentPath();

/**
 * @brief Spawns the application detached with stdout/stderr in a log file.
 * @return The child pid, or std::nullopt with @p error set.
 */
std::optional<int> SpawnDetached(const std::filesystem::path& app,
                                 const std::vector<std::string>& arguments,
                                 const std::filesystem::path& logPath,
                                 std::string& error);

/// Builds `--session <name> [--project <path>]`.
std::vector<std::string> BuildAppArguments(const std::string& session,
                                          const std::optional<std::string>& project);

} // namespace neurus::cli
