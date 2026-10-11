/**
 * @file Preferences.h
 * @brief Application-layer user preferences persisted to ~/.neurus/preferences.json.
 *
 * Owned exclusively by the Application: the active UI language and the
 * render-loop target FPS. Pure data + cereal JSON persistence, fully Qt-free
 * (the home directory comes from the platform layer).
 */

#pragma once

#include <string>

namespace neurus {

struct Preferences
{
	/**
	 * @brief Outcome of Load() - a missing file is not an error, a corrupt one is.
	 */
	enum class LoadResult
	{
		Ok,       ///< File read and parsed; fields hold the stored values.
		Missing,  ///< No file at that path; fields untouched.
		Corrupt   ///< File exists but failed to parse; fields untouched.
	};

	/** @brief Active UI language code ("en", "zh_CN"). "auto" = system-detect. */
	std::string language = "auto";

	/** @brief Render-loop target FPS (0 = unlimited, run as fast as possible). */
	int targetFps = 60;

	/** @brief Absolute path of the preferences file (~/.neurus/preferences.json). */
	static std::string DefaultPath();

	/**
	 * @brief Loads settings from @p path. Never throws, never partially applies.
	 *
	 * Parsing happens into a default-constructed Preferences, adopted only on
	 * success, so a truncated or schema-drifted file leaves the current values
	 * untouched and a missing key resolves to its in-class default.
	 * @param path Filesystem path (see DefaultPath()).
	 * @return Ok, Missing, or Corrupt (see LoadResult).
	 */
	LoadResult Load(const std::string& path);

	/**
	 * @brief Renames @p path to "<path>.bak" so a corrupt file survives.
	 *
	 * Called instead of a save when Load() reports Corrupt; an existing .bak
	 * is replaced.
	 * @param path Filesystem path of the file to move aside.
	 * @return true if the file was moved, false if it could not be (logged).
	 */
	static bool Backup(const std::string& path);

	/**
	 * @brief Saves settings to @p path, creating the parent directory.
	 * @param path Filesystem path (see DefaultPath()).
	 * @return true on success, false on I/O failure.
	 */
	bool Save(const std::string& path) const;
};

} // namespace neurus
