/**
 * @file Preferences.h
 * @brief Application-layer user preferences persisted to ~/.neurus/preferences.json.
 *
 * Preferences are app-level (NOT project-level) settings, owned and managed
 * exclusively by the Application: the active UI language, the render-loop
 * target FPS, and — in the future — options such as CUDA enablement, theme,
 * or shortcut schemes. The UI layer never sees this type: the Application
 * seeds the Preferences dialog with plain values (language, target FPS,
 * file path) and receives change requests back through UIEvents signals.
 *
 * Architecture:
 * - Pure data struct + cereal JSON persistence, fully Qt-free (the home
 *   directory comes from the Platform layer's HomeDirectory(), not QDir).
 * - Loading never throws and never mutates the fields on failure. "missing"
 *   and "corrupt" are reported separately (see LoadResult): a missing file is
 *   a first run and is answered by writing defaults, while a corrupt file must
 *   NOT be silently overwritten — the caller preserves it (see Backup()).
 * - "auto" language means "follow the system UI language"; resolving it to a
 *   concrete code is I18n's job (setLanguage() accepts the sentinel), so this
 *   data type — and the value on disk — stay free of any UI dependency.
 */

#pragma once

#include <string>

namespace neurus {

struct Preferences
{
	/**
	 * @brief Outcome of Load() — a missing file is not an error, a corrupt one is.
	 *
	 * The distinction matters because the caller's response differs: Missing is
	 * a first run and is answered by saving defaults, while Corrupt must never
	 * be answered by a save (that would destroy the user's settings before
	 * anyone could look at them).
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

	/**
	 * @brief Absolute path of the preferences file (~/.neurus/preferences.json).
	 */
	static std::string DefaultPath();

	/**
	 * @brief Loads settings from @p path. Never throws, never partially applies.
	 *
	 * Parsing happens into a scratch copy, so a file that is truncated or has
	 * drifted schema leaves the current values exactly as they were.
	 *
	 * @param path Filesystem path (see DefaultPath()).
	 * @return Ok, Missing, or Corrupt (see LoadResult).
	 */
	LoadResult Load(const std::string& path);

	/**
	 * @brief Renames @p path to "<path>.bak" so a corrupt file survives.
	 *
	 * Called instead of a save when Load() reports Corrupt: the bad file is kept
	 * for inspection, and the next save writes a fresh file rather than
	 * clobbering evidence. An existing .bak is replaced.
	 *
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
