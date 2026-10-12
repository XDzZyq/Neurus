/**
 * @file ScriptEngineImpl.h
 * @brief PIMPL body of `neurus::ScriptEngine`.
 *
 * Includes `<pybind11/embed.h>` and `<Python.h>` and therefore must only be
 * included by `ScriptEngine.cpp` - never by another layer or translation unit
 * (design section 4.1). All interpreter lifecycle and execution logic lives
 * here so the Python dependency is confined to one translation unit.
 */

#pragma once

// pybind11 first: it configures the Python headers before including them.
#include <pybind11/embed.h>

#include <Python.h>

#include <string>

#include "core/Log.h"
#include "script/ScriptEngine.h"
#include "script/neurus_python_config.h"

namespace neurus
{

namespace script
{
/**
 * @brief Module-registration anchor defined in `bindings/Module.cpp` (later
 *        task). Declared here so the engine can force the embedded-module
 *        translation unit to stay linked out of the static archive.
 */
void ForceLinkBindings();
}  // namespace script

/**
 * @brief Out-of-line definition of the `ScriptEngine` PIMPL.
 */
struct ScriptEngine::Impl
{
	/**
	 * @brief Initialize CPython in isolated mode against the base interpreter.
	 * @param envPath Managed virtual environment to attach, or empty.
	 * @return true when the interpreter is ready.
	 */
	bool Init(const std::string& envPath)
	{
		if (m_ready)
		{
			return true;
		}

		auto statusText = [](const PyStatus& status) -> const char*
		{
			return status.err_msg != nullptr ? status.err_msg : "(no message)";
		};

		PyConfig config;
		PyConfig_InitIsolatedConfig(&config);
		// Isolated mode (`-I`) still processes `site`/`.pth` when requested, so
		// the managed environment can be layered on top of the base.
		config.site_import = 1;

		PyStatus status = PyConfig_SetBytesString(&config, &config.home, NEURUS_PYTHON_HOME);
		if (PyStatus_Exception(status))
		{
			PyConfig_Clear(&config);
			NEURUS_ERR("ScriptEngine: failed to set Python home '" << NEURUS_PYTHON_HOME
			          << "': " << statusText(status));
			return false;
		}

		status = Py_InitializeFromConfig(&config);
		PyConfig_Clear(&config);
		if (PyStatus_Exception(status))
		{
			NEURUS_ERR("ScriptEngine: Py_InitializeFromConfig failed: " << statusText(status));
			return false;
		}

		// Bindings (later task) are registered via PYBIND11_EMBEDDED_MODULE and
		// must be present before the first import.
		// script::ForceLinkBindings();

		if (!envPath.empty())
		{
			AttachEnvironment(envPath);
		}

		m_ready = true;
		return true;
	}

	/// @brief Finalize CPython exactly once. Safe to call repeatedly.
	void Shutdown()
	{
		if (!m_ready)
		{
			return;
		}

		m_ready = false;
		// Never invoked from a finalizer/atexit path, and guarded above so a
		// second call is a no-op (design section 8).
		Py_FinalizeEx();
	}

	/// @brief True between a successful `Init()` and `Shutdown()`.
	bool IsReady() const
	{
		return m_ready;
	}

	/**
	 * @brief Execute a Python snippet, logging (never throwing) on failure.
	 * @param code UTF-8 Python source.
	 */
	void RunString(const std::string& code)
	{
		if (!m_ready)
		{
			NEURUS_ERR("ScriptEngine::RunString: scripting is not initialized");
			return;
		}

		pybind11::gil_scoped_acquire gil;
		try
		{
			pybind11::exec(code);
		}
		catch (const pybind11::error_already_set& e)
		{
			// Full traceback capture and console routing arrive in a later task;
			// for now the error is surfaced and never escapes to the host.
			NEURUS_ERR("ScriptEngine::RunString failed: " << e.what());
		}
	}

private:
	/**
	 * @brief Attach a managed virtual environment to the base interpreter.
	 * @param envPath Environment root (the directory that owns `pyvenv.cfg`).
	 *
	 * Uses `site.addsitedir()` plus pinning `sys.executable`/`sys.prefix`, the
	 * standard workaround for embedding into a venv (CPython #66409). A missing
	 * environment is tolerated: the interpreter stays isolated on the base.
	 */
	void AttachEnvironment(const std::string& envPath)
	{
		try
		{
			pybind11::gil_scoped_acquire gil;
			pybind11::dict scope;
			scope["neurus_env"] = envPath;
			pybind11::exec(
				"import os, sys, site, sysconfig\n"
				"env = os.path.abspath(neurus_env)\n"
				"site.addsitedir(sysconfig.get_path('purelib', vars={'base': env, 'platbase': env}))\n"
				"sys.executable = (os.path.join(env, 'Scripts', 'python.exe') if os.name == 'nt'\n"
				"                  else os.path.join(env, 'bin', 'python'))\n"
				"sys.prefix = env\n",
				scope, scope);
		}
		catch (const pybind11::error_already_set& e)
		{
			NEURUS_ERR("ScriptEngine: failed to attach Python env '" << envPath << "': " << e.what());
		}
	}

	bool m_ready = false;
};

}  // namespace neurus
