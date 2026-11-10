#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace neurus::platform
{

/**
 * @brief Creates a file exclusively and writes @p contents.
 * @return False when the file already exists or the write fails.
 */
bool CreateExclusiveFile(const std::filesystem::path& path, std::string_view contents);

/**
 * @brief Restricts a file or directory to the current user.
 * @param path File or directory to secure.
 * @param directory True when @p path is a directory.
 * @param error Receives a reason on failure.
 *
 * No-op on Windows, where the per-user profile already carries the ACL.
 */
bool RestrictToOwner(const std::filesystem::path& path, bool directory, std::string& error);

} // namespace neurus::platform
