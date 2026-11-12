/**
 * @file ScriptImports.h
 * @brief Import recording, startup preload and hot reload for the interpreter.
 *
 * The recorder owns a private Python helper namespace - never inserted into
 * `sys.modules` - that installs a `sys.meta_path` finder wrapping each module
 * loader in a timing loader. `ScriptEngineImpl.h` holds one of these, so the
 * engine gets preload, module reporting and hot reload without widening its
 * Python-owning surface: this header is plain C++ and pybind11 stays in
 * `ScriptImports.cpp`.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "script/ScriptEngine.h"

namespace neurus
{

/** @brief One module reloaded by a hot-reload pass. */
struct ReloadedModule
{
	std::string name;           ///< Dotted module name.
	double      seconds = 0.0;  ///< Measured reload time.
};

/** @brief Result of one hot-reload pass over the recorded project modules. */
struct ReloadOutcome
{
	bool                        ok = true;  ///< False when a reload raised.
	std::vector<ReloadedModule> reloaded;   ///< Modules re-executed, in load order.
	std::string                 error;      ///< Names the failing module when !ok.
};

/**
 * @brief Records imports of the embedded interpreter and reloads changed modules.
 *
 * Methods that touch Python require an initialized interpreter; the engine
 * calls them from its main-thread run path. @ref SetReloadRoots is safe before
 * @ref Install and applies its roots once the recorder is installed.
 */
class ScriptImportRecorder
{
public:
	ScriptImportRecorder();
	~ScriptImportRecorder();

	ScriptImportRecorder(const ScriptImportRecorder&) = delete;
	ScriptImportRecorder& operator=(const ScriptImportRecorder&) = delete;

	/**
	 * @brief Create the helper namespace and install the meta-path finder.
	 *
	 * Idempotent; a failure is logged and leaves the recorder uninstalled, so
	 * the engine still runs without recording.
	 */
	void Install();

	/// @brief Drop every Python reference; call before `Py_FinalizeEx`.
	void Release();

	/// @brief True between a successful Install() and Release().
	bool IsInstalled() const;

	/**
	 * @brief Import one package now, or report that it is already resident.
	 * @param package Dotted package name.
	 * @return The outcome; an import failure is returned, never thrown.
	 */
	ScriptPreloadResult Preload(const std::string& package);

	/**
	 * @brief Report the modules resident in `sys.modules`, in load order.
	 * @return At most the recorder's reporting bound, tracked modules first.
	 */
	std::vector<ScriptModuleInfo> LoadedModules();

	/**
	 * @brief Replace the reload roots; paths are normalized once.
	 * @param roots Directories whose modules may be reloaded.
	 */
	void SetReloadRoots(const std::vector<std::string>& roots);

	/// @return The normalized reload roots currently configured.
	std::vector<std::string> ReloadRoots() const;

	/**
	 * @brief Reload project-local modules whose source mtime changed.
	 *
	 * Walks the recorded modules in load order. A module is reloaded only when
	 * its origin is under a reload root, the file exists, and its
	 * `st_mtime_ns` is newer than the recorded value. A failure stops the pass
	 * and names the module.
	 *
	 * @param extraRoots Additional roots for this pass (the script's directory).
	 * @return The modules reloaded, or the failure naming the module.
	 */
	ReloadOutcome ReloadChanged(const std::vector<std::string>& extraRoots);

private:
	/// @brief Push the stored roots into the Python helper namespace.
	void ApplyReloadRoots();

	struct Impl;
	/// Created by Install(), after the interpreter exists: pybind11 members
	/// must not be constructed before Py_InitializeFromConfig.
	std::unique_ptr<Impl> m_impl;
	std::vector<std::string> m_roots;  ///< Roots, safe before Install().
};

}  // namespace neurus
