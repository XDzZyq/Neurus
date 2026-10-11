#pragma once

/**
 * @file PlatformPaths.h
 * @brief OS-level filesystem path helpers (Qt-free, standard library only).
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

} // namespace neurus
