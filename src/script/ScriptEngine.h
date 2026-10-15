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
#include <string>

#include "core/ScriptConsole.h"
#include "script/ScriptContext.h"

namespace neurus
{

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

	/**
	 * @brief Execute a Python snippet on the main thread.
	 * @param code UTF-8 Python source.
	 * @param ctx Read-only view for this run (borrowed, may be null). The caller
	 *            builds it fresh per run; the engine stores nothing.
	 * @note `stdout`/`stderr` are captured into the console buffer; a Python
	 *       error is buffered and logged but never propagates to the host.
	 */
	void RunString(const std::string& code, const ScriptContext* ctx = nullptr);

	/**
	 * @brief Execute a Python file on the main thread.
	 * @param path Filesystem path to a UTF-8 Python source file.
	 * @param ctx Read-only view for this run (borrowed, may be null).
	 * @note Reads the file, then runs it under the same capture/error path as
	 *       `RunString`; a missing file or Python error is logged but never
	 *       propagates to the host.
	 */
	void RunFile(const std::string& path, const ScriptContext* ctx = nullptr);

	/**
	 * @brief Queue a snippet to run at the next `ExecutePending()`.
	 * @param code UTF-8 Python source.
	 */
	void Submit(const std::string& code);

	/**
	 * @brief Queue a Python file to run at the next `ExecutePending()`.
	 * @param path Filesystem path to a UTF-8 Python source file.
	 */
	void SubmitFile(const std::string& path);

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
