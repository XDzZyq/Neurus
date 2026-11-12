/**
 * @file ScriptEngine.h
 * @brief Embedded CPython runtime owned by the Application layer.
 *
 * `ScriptEngine` initializes an isolated CPython interpreter (see
 * `ScriptEngineImpl.h`), executes snippets on the main thread, and tears the
 * interpreter down. It is PIMPL-based: no Python or pybind11 header is ever
 * exposed through this interface, so the Script layer stays just another
 * consumer of `neurus_editor`/`neurus_core`/`neurus_scene`.
 *
 * @note Main-thread only, like `Scene` and the controller layer (design
 *       section 8). Construction does not initialize Python; call
 *       `Initialize()` explicitly.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "script/ScriptConsole.h"
#include "script/ScriptContext.h"
#include "script/ScriptPolicy.h"

namespace neurus
{

class IEventQueue;

/**
 * @brief Outcome of the most recent run.
 *
 * `NotReady` also covers the state before the first run: the engine has not
 * executed anything yet.
 */
enum class ScriptRunStatus
{
	Ok,               ///< The run completed without a Python exception.
	ScriptError,      ///< Ordinary Python exception (or a host-side failure).
	SandboxViolation, ///< The policy refused an operation during the run.
	Timeout,          ///< The wall-clock watchdog interrupted the run.
	NotReady          ///< No interpreter, or no run yet.
};

/**
 * @brief Owns the embedded CPython interpreter and executes Python code.
 */
class ScriptEngine
{
public:
	ScriptEngine();
	~ScriptEngine();

	ScriptEngine(const ScriptEngine&) = delete;
	ScriptEngine& operator=(const ScriptEngine&) = delete;

	/**
	 * @brief Attach the event queue scripted mutations are dispatched to.
	 *
	 * Without a queue every setter raises, so the engine is attached before
	 * `Initialize()` in the application and before the first run in a test.
	 *
	 * @param queue Borrowed editor queue; must outlive the engine.
	 */
	void SetEventQueue(IEventQueue* queue);

	/**
	 * @brief Initialize CPython in isolated mode.
	 * @param envPath Managed virtual environment to attach on top of the base
	 *                interpreter, or empty for the isolated base only.
	 * @return true when the interpreter is ready; false after logging the
	 *         failure via `NEURUS_ERR`.
	 */
	bool Initialize(const std::string& envPath = {});

	/// @brief Finalize the interpreter. Idempotent; safe from the destructor.
	void Shutdown();

	/// @brief True between a successful `Initialize()` and `Shutdown()`.
	bool IsReady() const;

	/// @return Python runtime version, or an empty string before initialization.
	std::string PythonVersion() const;

	/// @return Managed environment path requested at initialization, or empty.
	std::string EnvironmentPath() const;

	/**
	 * @brief Evaluate one Python expression and return its `repr`.
	 *
	 * Runs through the same capture and run-boundary path as @ref RunString, so
	 * side effects are one undo entry and captured output lands in the console.
	 *
	 * @param expression Python expression source.
	 * @param ctx Read-only view for this run (borrowed, may be null).
	 * @param policy Restrictions for this run; null and Trusted both run
	 *               unrestricted.
	 * @return The expression result as `repr`, or an empty string when the
	 *         evaluation raised or the engine is not ready.
	 */
	std::string EvalString(const std::string& expression, const ScriptContext* ctx = nullptr,
	                       const ScriptPolicy* policy = nullptr);

	/**
	 * @brief Execute a Python snippet and report whether it raised.
	 *
	 * Same capture and run-boundary path as RunString, but the host can tell a
	 * Python exception from ordinary stderr output.
	 *
	 * @param code UTF-8 Python source.
	 * @param ctx Read-only view for this run (borrowed, may be null).
	 * @param policy Restrictions for this run; null and Trusted both run
	 *               unrestricted.
	 * @return True when the snippet completed without a Python exception.
	 */
	bool RunStringChecked(const std::string& code, const ScriptContext* ctx = nullptr,
	                      const ScriptPolicy* policy = nullptr);

	/**
	 * @brief Execute a Python file and report whether it raised.
	 * @param path Filesystem path to a UTF-8 Python source file.
	 * @param ctx Read-only view for this run (borrowed, may be null).
	 * @param policy Restrictions for this run; null and Trusted both run
	 *               unrestricted.
	 * @return False when the file cannot be read or the script raised.
	 */
	bool RunFileChecked(const std::string& path, const ScriptContext* ctx = nullptr,
	                    const ScriptPolicy* policy = nullptr);

	/**
	 * @brief Evaluate one expression and report failure explicitly.
	 * @param expression Python expression source.
	 * @param ctx Read-only view for this run (borrowed, may be null).
	 * @param policy Restrictions for this run; null and Trusted both run
	 *               unrestricted.
	 * @return The expression result as `repr`, or std::nullopt when it raised.
	 */
	std::optional<std::string> EvalStringChecked(const std::string& expression,
	                                             const ScriptContext* ctx = nullptr,
	                                             const ScriptPolicy* policy = nullptr);

	/**
	 * @brief Execute a Python snippet on the main thread.
	 * @param code UTF-8 Python source.
	 * @param ctx Read-only view for this run (borrowed, may be null).
	 * @param policy Restrictions for this run; null and Trusted both run
	 *               unrestricted.
	 * @note `stdout`/`stderr` are captured into the console buffer; a Python
	 *       error is buffered and logged but never propagates to the host.
	 */
	void RunString(const std::string& code, const ScriptContext* ctx = nullptr,
	               const ScriptPolicy* policy = nullptr);

	/**
	 * @brief Execute a Python file on the main thread.
	 * @param path Filesystem path to a UTF-8 Python source file.
	 * @param ctx Read-only view for this run (borrowed, may be null).
	 * @param policy Restrictions for this run; null and Trusted both run
	 *               unrestricted.
	 * @note Reads the file, then runs it under the same capture/error path as
	 *       `RunString`; a missing file or Python error is logged but never
	 *       propagates to the host.
	 */
	void RunFile(const std::string& path, const ScriptContext* ctx = nullptr,
	             const ScriptPolicy* policy = nullptr);

	/// @return Outcome of the most recent run (NotReady before the first one).
	ScriptRunStatus LastRunStatus() const;

	/// @return Human-readable detail for the last run, empty on success.
	std::string LastRunMessage() const;

	/// @return The policy the last run executed under.
	ScriptPolicy LastPolicy() const;

	/**
	 * @brief Queue a snippet to run at the next `ExecutePending()`.
	 * @param code UTF-8 Python source.
	 * @note This overload runs trusted; use the policy overload to restrict it.
	 */
	void Submit(const std::string& code);

	/**
	 * @brief Queue a snippet under a policy for the next `ExecutePending()`.
	 * @param code UTF-8 Python source.
	 * @param policy Restrictions applied when the run executes.
	 */
	void Submit(const std::string& code, const ScriptPolicy& policy);

	/**
	 * @brief Queue a Python file to run at the next `ExecutePending()`.
	 * @param path Filesystem path to a UTF-8 Python source file.
	 * @note This overload runs trusted; use the policy overload to restrict it.
	 */
	void SubmitFile(const std::string& path);

	/**
	 * @brief Queue a Python file under a policy for the next `ExecutePending()`.
	 * @param path Filesystem path to a UTF-8 Python source file.
	 * @param policy Restrictions applied when the run executes.
	 */
	void SubmitFile(const std::string& path, const ScriptPolicy& policy);

	/**
	 * @brief Run every queued script in submission order, then clear the queue.
	 *
	 * The caller generates @p ctx fresh for the batch (borrowed only for the
	 * call). Each queued script runs through the same capture/error path as
	 * `RunString`; a script that submits another run is executed on the next
	 * `ExecutePending()`.
	 *
	 * @param ctx Read-only view for these runs.
	 */
	void ExecutePending(const ScriptContext& ctx);

	/**
	 * @brief The engine's console buffer (captured output, errors, results).
	 * @return A reference to the buffer owned by the PIMPL body; its lifetime
	 *         matches the engine.
	 */
	ScriptConsoleBuffer& Console();

private:
	struct Impl;  ///< Python/pybind11-owning body; defined in ScriptEngineImpl.h
	std::unique_ptr<Impl> m_impl;
};

}  // namespace neurus
