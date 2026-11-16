#pragma once

#include <optional>
#include <string>
#include <vector>

namespace neurus::cli
{

/**
 * @brief Builds `--session <name> [--project <path>] [--preload NAME]...`.
 *
 * A preload is passed as a repeated `--preload` pair, which `AppArgs` accepts
 * either repeatedly or comma-separated.
 */
std::vector<std::string> BuildAppArguments(
	const std::string& session,
	const std::optional<std::string>& project,
	const std::vector<std::string>& preload = {});

} // namespace neurus::cli
