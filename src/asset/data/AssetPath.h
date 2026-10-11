/**
 * @file AssetPath.h
 * @brief Asset path resolution helper (asset layer).
 *
 * Relative "res/..." paths resolve against the compile-time NEURUS_RES_DIR,
 * with a working-directory fallback for tests; absolute paths pass through.
 * Project files always store relative paths, so they stay portable.
 */

#pragma once

#include <string>

namespace neurus
{

/**
 * @brief Resolves a resource path to an existing file.
 * @param path "res/..." relative path, CWD-relative path, or absolute path.
 * @return The first existing candidate, or @p path unchanged if none is found.
 */
std::string ResolveAssetPath(const std::string& path);

/**
 * @brief Converts a path into the portable form stored in project files.
 * @param path Absolute path (e.g. from a file dialog) or already-relative path.
 * @return "res/..." when @p path lies under the res dir, otherwise @p path
 *         unchanged.
 * @note Inverse of ResolveAssetPath for files inside the res dir.
 */
std::string MakePortableAssetPath(const std::string& path);

} // namespace neurus
