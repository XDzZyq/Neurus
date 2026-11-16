#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace neurus::cli
{

/// @return `NEURUS_SESSION`, or std::nullopt when unset.
std::optional<std::string> EnvironmentSession();

/// @return The current terminal's stable identity; empty when unknown.
std::string TerminalIdentity();

/// @return Directory holding per-terminal session bindings.
std::filesystem::path TerminalBindingDirectory();

/// @return This terminal's binding file path.
std::filesystem::path TerminalBindingPath();

/// @return The session bound to this terminal, or std::nullopt.
std::optional<std::string> ReadTerminalBinding();

/**
 * @brief Binds @p session to this terminal.
 * @param session Live session name.
 * @param error Receives a reason on failure.
 * @return True on success.
 */
bool WriteTerminalBinding(const std::string& session, std::string& error);

/// @brief Removes this terminal's binding.
bool ClearTerminalBinding(std::string& error);

/// @brief Removes the binding when it names @p session; silent otherwise.
void ClearTerminalBindingIfMatches(const std::string& session);

} // namespace neurus::cli
