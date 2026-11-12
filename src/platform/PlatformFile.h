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
 * @brief Creates or truncates @p path owner-only and writes @p contents.
 * @param path File to write.
 * @param contents Bytes to store; a short write is reported as a failure.
 * @param error Receives a reason on failure.
 *
 * The file is readable and writable by the owner only from the moment it
 * exists - unlike a save followed by RestrictToOwner(), which leaves a window
 * where another user can open it. A partial file is left behind on failure so
 * the caller can remove or retry it.
 * @return True when every byte was written.
 */
bool WriteOwnerOnlyFile(const std::filesystem::path& path, std::string_view contents,
                        std::string& error);

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
