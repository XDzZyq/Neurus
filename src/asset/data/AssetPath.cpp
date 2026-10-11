/**
 * @file AssetPath.cpp
 * @brief Asset path resolution (see AssetPath.h).
 */

#include "asset/data/AssetPath.h"

#include <filesystem>

namespace neurus
{

namespace
{

/** @brief True for Windows drive (C:) or absolute root (/ or \) paths. */
bool IsAbsolutePath(const std::string& path)
{
	return (!path.empty() && (path[0] == '/' || path[0] == '\\'))
		|| (path.size() > 1 && path[1] == ':');
}

} // anonymous namespace

std::string ResolveAssetPath(const std::string& path)
{
	if (path.empty() || IsAbsolutePath(path))
		return path;

	// Strip a leading "res/" prefix: "res/obj/sphere.obj" resolves to
	// NEURUS_RES_DIR + "obj/sphere.obj".
	if (path.rfind("res/", 0) == 0)
	{
		std::string resolved = std::string(NEURUS_RES_DIR) + path.substr(4);
		if (std::filesystem::exists(resolved))
			return resolved;
	}

	// Fallback: working-directory-relative with a few up-walks (tests run
	// from build or deeper, where res/ is copied).
	for (const auto& prefix : {"", "../", "../../", "../../../"})
	{
		std::string resolved = std::string(prefix) + path;
		if (std::filesystem::exists(resolved))
			return resolved;
	}

	return path; // not found
}

std::string MakePortableAssetPath(const std::string& path)
{
	if (path.empty() || !IsAbsolutePath(path))
		return path;

	std::error_code ec;
	const auto resDir = std::filesystem::weakly_canonical(std::filesystem::path(NEURUS_RES_DIR), ec);
	if (ec)
		return path;

	const auto abs = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
	if (ec)
		return path;

	// Outside the res dir ("..") -> not portable, keep the absolute path.
	const auto rel = abs.lexically_relative(resDir);
	if (rel.empty() || rel.begin()->string() == "..")
		return path;

	return "res/" + rel.generic_string();
}

} // namespace neurus
