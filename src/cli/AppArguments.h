#pragma once

#include <optional>
#include <string>
#include <vector>

namespace neurus::cli
{

/// Builds `--session <name> [--project <path>]`.
std::vector<std::string> BuildAppArguments(const std::string& session,
                                          const std::optional<std::string>& project);

} // namespace neurus::cli
