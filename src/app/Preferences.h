/**
 * @file Preferences.h
 * @brief Application-layer user preferences persisted to ~/.neurus/preferences.json.
 *
 * Owned exclusively by the Application: the active UI language, the
 * render-loop target FPS and the startup script preload list. Pure data +
 * cereal JSON persistence, fully Qt-free (the home directory comes from the
 * platform layer).
 */

#pragma once

#include <string>
#include <vector>

#include "script/ScriptPolicy.h"

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

	/**
	 * @brief Packages imported at startup, before the first frame.
	 *
	 * A heavy package pays its import cost once here instead of on the first
	 * script that needs it. `--preload` overrides this list for one launch and
	 * `NEURUS_SCRIPT_PRELOAD` is the fallback when neither is set; see the
	 * Scripting documentation.
	 */
	std::vector<std::string> scriptPreload;

	// --- Script sandbox: the host's single policy for external runs ----------
	// UI runs are trusted. CLI/agent script commands always execute under this
	// policy, and the CLI cannot override it. Empty roots mean the project
	// directory (the directory of the open project, or the process cwd).

	/** @brief Read roots; empty means the project directory. */
	std::vector<std::string> scriptSandboxReadRoots;
	/** @brief Write roots; empty means the project directory. */
	std::vector<std::string> scriptSandboxWriteRoots;
	/** @brief Modules an external sandboxed run may not import. */
	std::vector<std::string> scriptSandboxBlockedModules;
	/** @brief Whether a sandboxed run may open sockets. */
	bool scriptSandboxAllowNetwork = false;
	/** @brief Whether a sandboxed run may create processes. */
	bool scriptSandboxAllowSubprocess = false;
	/** @brief Whether a sandboxed run may use ctypes. */
	bool scriptSandboxAllowCtypes = false;
	/** @brief Wall-clock limit in seconds; 0 = unlimited. */
	int scriptSandboxTimeLimitSeconds = 30;
	/** @brief Address-space limit in MB; 0 = unlimited. */
	int scriptSandboxMemoryLimitMb = 0;

	/**
	 * @brief Builds the sandbox policy external script commands execute under.
	 * @param projectRoot Project directory substituted when a root list is empty.
	 */
	ScriptPolicy MakeScriptPolicy(const std::string& projectRoot) const;

	/**
	 * @brief Value equality over every persisted field.
	 *
	 * Defaulted so a new field is never forgotten: the Application saves on
	 * shutdown only when the live values differ from the loaded ones.
	 */
	bool operator==(const Preferences&) const = default;

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
