#pragma once

/**
 * @file PlatformPaths.h
 * @brief OS-level filesystem path helpers (Qt-free).
 *
 * Architecture: Platform layer — leaf dependency, std only. The Application
 * layer (e.g. Preferences) uses HomeDirectory() without any Qt or #ifdef.
 */

#include <filesystem>

namespace neurus {

/**
 * @brief The current user's home directory.
 *
 * Mirrors QDir::homePath(): $HOME on POSIX, %USERPROFILE% on Windows. Falls
 * back to the current working directory if neither variable is set.
 * @return The home directory (absolute when the environment variable is set).
 */
std::filesystem::path HomeDirectory();

} // namespace neurus
