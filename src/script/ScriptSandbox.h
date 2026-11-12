/**
 * @file ScriptSandbox.h
 * @brief In-process guardrail installed into the embedded interpreter.
 *
 * Python/pybind11 internals: only `ScriptEngineImpl.h` includes this header,
 * so the rest of the codebase never sees an audit hook, a meta-path finder or a
 * watchdog thread.
 *
 * The sandbox is one `sys.addaudithook` hook plus an import gate plus an
 * optional wall-clock watchdog. It is a guardrail against accidental damage,
 * not a security boundary: code running in the same interpreter can reach
 * objects the hook itself uses.
 */

#pragma once

#include <pybind11/embed.h>

#include <Python.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "script/ScriptEngine.h"

namespace neurus
{

/**
 * @brief Owns the Python-side policy hook and its watchdog thread.
 *
 * Every method that touches a Python object must be called with the GIL held
 * and on the main thread. `JoinWatchdog()` is the exception: it must be called
 * **after** the GIL is released, and `StopWatchdog()` only signals.
 */
class ScriptSandbox
{
public:
	ScriptSandbox() = default;

	/**
	 * @brief Destructor; assumes the GIL is held (engine main thread).
	 *
	 * `ScriptEngineImpl::Shutdown` calls @ref Reset before `Py_FinalizeEx`, so a
	 * normal teardown never reaches the destructor with live Python objects.
	 */
	~ScriptSandbox();

	ScriptSandbox(const ScriptSandbox&) = delete;
	ScriptSandbox& operator=(const ScriptSandbox&) = delete;

	/**
	 * @brief Executes the helper source once and exposes the exceptions.
	 *
	 * The helper dict is private and is never inserted into `sys.modules`.
	 * @return true when the audit hook and import gate are installed.
	 */
	bool Install();

	/// @return True after a successful `Install()`.
	bool IsInstalled() const
	{
		return m_installed;
	}

	/**
	 * @brief Makes @p policy the active rule set for the interpreter.
	 * @return false when applying the policy failed; the policy is then cleared.
	 */
	bool SetPolicy(const ScriptPolicy& policy);

	/// @brief Clears the active policy; every hook check becomes a pass.
	void ClearPolicy();

	/**
	 * @brief Classifies a Python exception against the sandbox exception types.
	 * @return Timeout, SandboxViolation, or ScriptError.
	 */
	ScriptRunStatus Classify(const pybind11::error_already_set& error) const;

	/**
	 * @brief Starts the wall-clock watchdog for one run.
	 * @param limit Timeout, or a non-positive value to run without a watchdog.
	 * @param target Thread state that receives the asynchronous exception.
	 */
	void BeginWatchdog(std::chrono::milliseconds limit, PyThreadState* target);

	/// @brief Signals the watchdog to stop, and clears a pending injection.
	void StopWatchdog();

	/// @brief Joins the watchdog thread. Must be called without the GIL.
	void JoinWatchdog();

	/**
	 * @brief Applies an in-process memory limit for the run.
	 *
	 * Implemented with Python's `resource` module; on a platform without it the
	 * call reports failure and the run continues unbounded.
	 * @param bytes Limit, or zero when the policy sets none.
	 * @return false when the limit is unsupported or could not be applied.
	 */
	bool ApplyMemoryLimit(std::uint64_t bytes);

	/// @brief Restores the limits saved by @ref ApplyMemoryLimit.
	void RestoreMemoryLimit();

	/**
	 * @brief Stops the watchdog and releases every Python object.
	 *
	 * Called with the GIL held, before `Py_FinalizeEx`.
	 */
	void Reset();

private:
	/// @brief Waits for the deadline, then raises the timeout in the target.
	void WatchdogLoop();

	pybind11::dict m_helper;               ///< Private namespace of the helper source.
	pybind11::object m_violation;          ///< `neurus._SandboxViolation`.
	pybind11::object m_importViolation;    ///< `neurus._SandboxImportViolation`.
	pybind11::object m_timeoutObject;      ///< `neurus._SandboxTimeout`.
	bool m_installed = false;

	std::thread             m_watchdog;
	std::mutex              m_watchdogMutex;
	std::condition_variable m_watchdogCv;
	bool                    m_watchdogStop = true;
	unsigned long           m_watchdogTargetId = 0;
	unsigned long           m_watchdogInjectedId = 0;
	std::chrono::steady_clock::time_point m_watchdogDeadline;
};

}  // namespace neurus
