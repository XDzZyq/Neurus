/**
 * @file ScriptPolicy.h
 * @brief Python-free description of the restrictions one script run executes under.
 *
 * A policy is a plain value: the engine copies it for the duration of a run, so
 * a caller may keep, reuse or outlive the object it passed. `Trusted` is the
 * absence of restrictions; `Sandboxed` is the in-process guardrail first cut -
 * an audit-hook and import gate inside the one CPython interpreter, not a
 * process boundary.
 *
 * @note This header is Python-free on purpose: the command layer, the CLI and
 *       the Application all build policies without seeing pybind11 or CPython.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace neurus
{

/// @brief Execution mode a script run is bound to.
enum class ScriptMode
{
	Trusted,    ///< No restriction; the historical behaviour.
	Sandboxed   ///< Filesystem, network, subprocess, ctypes and import rules apply.
};

/**
 * @brief Everything one run is allowed to do.
 *
 * Empty root lists deny every path, which is the fail-closed default: a policy
 * that names no root grants no filesystem access. `wallClockLimit` and
 * `memoryLimitBytes` are zero when unbounded.
 */
struct ScriptPolicy
{
	ScriptMode mode = ScriptMode::Trusted;
	std::vector<std::string> readRoots;       ///< Directories reads are confined to.
	std::vector<std::string> writeRoots;      ///< Directories writes are confined to.
	std::vector<std::string> blockedModules;  ///< Module names (or roots) refused at import.
	bool allowNetwork = true;                 ///< Socket connect/bind/name lookups.
	bool allowSubprocess = true;              ///< subprocess, os.system, exec and fork.
	bool allowCtypes = true;                  ///< Every `ctypes.*` audit event.
	std::chrono::milliseconds wallClockLimit{ 0 };  ///< 0 = unbounded.
	std::uint64_t memoryLimitBytes = 0;       ///< 0 = unbounded (POSIX best effort).

	/// @return The unrestricted policy every existing caller keeps using.
	static ScriptPolicy Trusted();

	/**
	 * @brief Builds the default guardrail policy for a project.
	 *
	 * Reads and writes are confined to @p projectRoot; network, subprocess and
	 * ctypes are refused; the wall clock is capped at 30 seconds; no module is
	 * blocked.
	 *
	 * @param projectRoot Directory that becomes both the read and write root.
	 *                    Normalization happens on the Python side; an empty
	 *                    string yields a policy that denies every path.
	 * @return The sandboxed policy.
	 */
	static ScriptPolicy Sandboxed(const std::string& projectRoot);

	/**
	 * @brief Wire name of a mode ("trusted" or "sandboxed").
	 * @param mode Mode to render.
	 * @return A stable, lower-case string; never null.
	 */
	static const char* ModeName(ScriptMode mode);
};

}  // namespace neurus
