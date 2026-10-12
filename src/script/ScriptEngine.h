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
	 * @note A Python error is logged but never propagates to the host.
	 */
	void RunString(const std::string& code);

private:
	struct Impl;  ///< Python/pybind11-owning body; defined in ScriptEngineImpl.h
	std::unique_ptr<Impl> m_impl;
};

}  // namespace neurus
