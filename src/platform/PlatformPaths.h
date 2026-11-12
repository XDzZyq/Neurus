#pragma once

/**
 * @file PlatformPaths.h
 * @brief Per-user filesystem paths (standard library only, no Vulkan).
 */

#include <filesystem>

namespace neurus {

/**
 * @brief The current user's home directory.
 *
 * $HOME on POSIX, %USERPROFILE% on Windows; falls back to the current
 * working directory if neither is set.
 * @return The home directory (absolute when the environment variable is set).
 */
std::filesystem::path HomeDirectory();

/**
 * @brief The per-user Neurus configuration directory.
 * @return HomeDirectory() / ".neurus".
 */
std::filesystem::path NeurusConfigDirectory();

/**
 * @brief The directory holding command-session registry entries.
 * @return NeurusConfigDirectory() / "sessions".
 */
std::filesystem::path SessionsDirectory();

} // namespace neurus
