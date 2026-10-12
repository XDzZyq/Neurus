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

#include <fstream>
#include <iterator>
#include <string>

#include "core/Log.h"
#include "script/ScriptEngine.h"
#include "script/bindings/Runtime.h"
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

		// The embedded `neurus` module registers itself from a static
		// initializer in `bindings/Module.cpp`. Referencing its anchor pins the
		// translation unit into the link so the registrar is present; it must
		// run before the interpreter, because pybind11 refuses to append a
		// module once Python is initialized.
		script::ForceLinkBindings();

		status = Py_InitializeFromConfig(&config);
		PyConfig_Clear(&config);
		if (PyStatus_Exception(status))
		{
			NEURUS_ERR("ScriptEngine: Py_InitializeFromConfig failed: " << statusText(status));
			return false;
		}

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
	 * @brief Execute a Python snippet, buffering output and logging failures.
	 * @param code UTF-8 Python source.
	 */
	void RunString(const std::string& code)
	{
		if (!m_ready)
		{
			NEURUS_ERR("scripting not initialized");
			return;
		}

		RunSource(code, "ScriptEngine::RunString");
	}

	/**
	 * @brief Read and execute a Python file, buffering output and logging.
	 * @param path Filesystem path to a UTF-8 Python source file.
	 */
	void RunFile(const std::string& path)
	{
		if (!m_ready)
		{
			NEURUS_ERR("scripting not initialized");
			return;
		}

		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			NEURUS_ERR("ScriptEngine::RunFile: cannot open '" << path << "'");
			return;
		}

		// Slurp the whole file: `py::exec` needs a complete source string, and
		// CPython decodes UTF-8 source itself.
		const std::string source((std::istreambuf_iterator<char>(file)),
		                         std::istreambuf_iterator<char>());
		RunSource(source, "ScriptEngine::RunFile");
	}

	/// @brief The buffer captured Python output and errors are written to.
	ScriptConsoleBuffer& Console()
	{
		return m_console;
	}

	/// @brief Store a copy of the read-only scene/config view for later runs.
	void SetContext(const ScriptContext& ctx)
	{
		m_context = ctx;
	}

private:
	/// @brief Shared run path: GIL, stdout/stderr capture, error surfacing.
	void RunSource(const std::string& source, const char* entryName)
	{
		pybind11::gil_scoped_acquire gil;
		// Publish the run's context to the bindings for exactly this run.
		ContextScope contextScope(&m_context);

		try
		{
			// Redirect before running so `print` and tracebacks land in the
			// buffer. The guard restores the original streams on both the
			// normal and the error path; it is destroyed (while the GIL is
			// still held) as this try scope unwinds.
			StreamRedirect redirect(m_console);
			pybind11::exec(source, pybind11::globals(), pybind11::globals());
		}
		catch (const pybind11::error_already_set& e)
		{
			ReportError(e, entryName);
		}
	}

	/// @brief Formats and routes a Python error to the console buffer and log.
	void ReportError(const pybind11::error_already_set& e, const char* entryName)
	{
		std::string trace = FormatTraceback(e);
		if (trace.empty())
		{
			trace = e.what();
		}

		m_console.Append(ScriptLineKind::Error, trace);
		NEURUS_ERR(entryName << " failed:\n" << trace);
	}

	/**
	 * @brief Renders an exception with Python's `traceback.format_exception`.
	 * @return The formatted traceback, or empty when formatting fails (the
	 *         caller then falls back to `e.what()`).
	 */
	std::string FormatTraceback(const pybind11::error_already_set& e)
	{
		try
		{
			pybind11::object traceback = pybind11::module_::import("traceback");
			pybind11::list lines =
				traceback.attr("format_exception")(e.type(), e.value(), e.trace());

			std::string text;
			for (auto line : lines)
			{
				text += line.cast<std::string>();
			}
			return text;
		}
		catch (const std::exception& formatError)
		{
			// Formatting is best-effort; `e.what()` still carries the type,
			// value and a condensed trace, so let the caller fall back to it.
			NEURUS_ERR("ScriptEngine: traceback formatting failed: " << formatError.what());
			return {};
		}
	}

	/**
	 * @brief RAII bracket that publishes the run's context to the bindings.
	 *
	 * Proxies reach the scene through `script::CurrentContext()` at attribute
	 * access time, so the context is published for the duration of one run and
	 * cleared on every exit path - success, Python error, or host exception -
	 * before the interpreter could observe a pointer into a finished run.
	 */
	class ContextScope
	{
	public:
		explicit ContextScope(const ScriptContext* context)
		{
			script::SetCurrentContext(context);
		}

		~ContextScope()
		{
			script::SetCurrentContext(nullptr);
		}

		ContextScope(const ContextScope&) = delete;
		ContextScope& operator=(const ContextScope&) = delete;
	};

	/**
	 * @brief RAII redirection of Python `sys.stdout`/`sys.stderr`.
	 *
	 * Constructed with the GIL held and before a run, it installs two writer
	 * objects that append to the engine's console buffer; the destructor
	 * restores the saved streams. It is destroyed before the
	 * `gil_scoped_acquire` guard, so restoration always happens with the GIL
	 * held and on every exit path (success, Python error, host exception).
	 */
	class StreamRedirect
	{
	public:
		explicit StreamRedirect(ScriptConsoleBuffer& console)
		{
			pybind11::object sys = pybind11::module_::import("sys");
			m_sys = sys;
			m_savedOut = sys.attr("stdout");
			m_savedErr = sys.attr("stderr");

			// Build both writers before touching `sys`, so a failure in either
			// MakeWriter leaves the interpreter's streams untouched.
			pybind11::object outWriter = MakeWriter(console, ScriptLineKind::Output);
			pybind11::object errWriter = MakeWriter(console, ScriptLineKind::Error);

			try
			{
				m_sys.attr("stdout") = outWriter;
				m_sys.attr("stderr") = errWriter;
			}
			catch (...)
			{
				// A half-installed redirect must not outlive the constructor,
				// since a throwing constructor never runs the destructor.
				try
				{
					m_sys.attr("stdout") = m_savedOut;
					m_sys.attr("stderr") = m_savedErr;
				}
				catch (const std::exception& restoreError)
				{
					NEURUS_ERR("ScriptEngine: failed to restore Python streams after a "
					           "redirect failure: " << restoreError.what());
				}
				throw;
			}
		}

		~StreamRedirect()
		{
			try
			{
				m_sys.attr("stdout") = m_savedOut;
				m_sys.attr("stderr") = m_savedErr;
			}
			catch (const std::exception& restoreError)
			{
				NEURUS_ERR("ScriptEngine: failed to restore Python streams: "
				           << restoreError.what());
			}
		}

		StreamRedirect(const StreamRedirect&) = delete;
		StreamRedirect& operator=(const StreamRedirect&) = delete;

	private:
		/// @brief Builds a minimal file-like object appending to `console`.
		static pybind11::object MakeWriter(ScriptConsoleBuffer& console,
		                                   ScriptLineKind kind)
		{
			// SimpleNamespace permits the attribute assignment that a bare
			// `object` and `io.StringIO` (read-only `write`) do not.
			pybind11::object writer =
				pybind11::module_::import("types").attr("SimpleNamespace")();
			writer.attr("write") = pybind11::cpp_function(
				[&console, kind](const std::string& text) -> int
				{
					console.Append(kind, text);
					return static_cast<int>(text.size());
				});
			writer.attr("flush") = pybind11::cpp_function([]() {});
			return writer;
		}

		pybind11::object m_sys;
		pybind11::object m_savedOut;
		pybind11::object m_savedErr;
	};

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
	ScriptConsoleBuffer m_console;
	ScriptContext m_context;  ///< Last context handed to `SetContext`.
};

}  // namespace neurus
