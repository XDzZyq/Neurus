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
#include <vector>

#include "core/Log.h"
#include "script/ScriptDispatch.h"
#include "script/ScriptEngine.h"
#include "script/bindings/Bindings.h"
#include "script/ScriptView.h"
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

		// An explicit path wins; otherwise the build-time default environment
		// (the uv-managed lib/.venv) is attached, or skipped when it is absent.
		AttachEnvironment(envPath.empty() ? std::string(NEURUS_PYTHON_ENV) : envPath);

		// Keep the module handle: the views were registered once and resolve
		// through the engine's ScriptView.
		try
		{
			m_neuru = pybind11::module_::import("neurus");
		}
		catch (const pybind11::error_already_set& e)
		{
			NEURUS_ERR("ScriptEngine: embedded 'neurus' module is unavailable: " << e.what());
			return false;
		}

		// Create the interpreter's view instances once. They hold a pointer to
		// the engine-owned view, whose address is stable for the interpreter's
		// lifetime; only the document it exposes changes per run.
		script::InstallNeurusViews(m_neuru, &m_view);

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
		// Drop the module handle before finalizing: its destructor decrefs, which
		// must not happen after the interpreter is gone.
		m_neuru = pybind11::module_();
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
	void RunString(const std::string& code, const ScriptContext* ctx)
	{
		if (!m_ready)
		{
			NEURUS_ERR("scripting not initialized");
			return;
		}

		RunSource(code, "ScriptEngine::RunString", ctx);
	}

	/**
	 * @brief Read and execute a Python file, buffering output and logging.
	 * @param path Filesystem path to a UTF-8 Python source file.
	 */
	void RunFile(const std::string& path, const ScriptContext* ctx)
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
		RunSource(source, "ScriptEngine::RunFile", ctx);
	}

	/// @brief The buffer captured Python output and errors are written to.
	ScriptConsoleBuffer& Console()
	{
		return m_console;
	}

	/// @brief Queue a snippet for the next ExecutePending().
	void Submit(const std::string& code)
	{
		m_pending.push_back(PendingRun{false, code});
	}

	/// @brief Queue a file for the next ExecutePending().
	void SubmitFile(const std::string& path)
	{
		m_pending.push_back(PendingRun{true, path});
	}

	/// @brief Run every queued script in order, then clear the queue.
	void ExecutePending(const ScriptContext& ctx)
	{
		if (m_pending.empty())
		{
			return;
		}

		// Swap out so a script that submits another run cannot mutate this batch.
		std::vector<PendingRun> batch;
		batch.swap(m_pending);

		for (const PendingRun& run : batch)
		{
			if (run.fromFile)
			{
				RunFile(run.text, &ctx);
			}
			else
			{
				RunString(run.text, &ctx);
			}
		}
	}

private:
	/// @brief Shared run path: GIL, stdout/stderr capture, error surfacing.
	void RunSource(const std::string& source, const char* entryName, const ScriptContext* ctx)
	{
		pybind11::gil_scoped_acquire gil;

		// Publish this run's document view. The scope republishes `m_current`
		// for the run and clears it on every exit path, so no Python object can
		// read a finished run's state.
		ContextScope scope(m_current, ctx);

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
	 * @brief Publishes one run's document context and clears it on exit.
	 *
	 * The engine owns the slot and the bindings resolve through it; nothing
	 * run-scoped is ever handed to Python, so a finished run cannot be read.
	 */
	class ContextScope
	{
	public:
		ContextScope(ScriptContext& slot, const ScriptContext* ctx)
			: m_slot(slot)
			, m_saved(slot)
		{
			m_slot = ctx != nullptr ? *ctx : ScriptContext{};
		}

		~ContextScope()
		{
			m_slot = m_saved;
		}

		ContextScope(const ContextScope&) = delete;
		ContextScope& operator=(const ContextScope&) = delete;

	private:
		ScriptContext& m_slot;
		ScriptContext  m_saved;  ///< Outer run's document, restored on exit (nested runs).
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

	/// @brief A run queued by Submit()/SubmitFile(), executed by ExecutePending().
	struct PendingRun
	{
		bool        fromFile = false;  ///< true = @ref text is a path; else source.
		std::string text;
	};

	bool m_ready = false;
	pybind11::module_ m_neuru;           ///< Embedded module; views registered once.
	ScriptConsoleBuffer m_console;
	ScriptDispatch m_dispatch;          ///< Read-only (no event queue) in this phase.
	ScriptContext m_current;            ///< Document published for the run in progress.
	script::ScriptView m_view{&m_current, &m_dispatch};  ///< Injected into the module once.
	std::vector<PendingRun> m_pending;  ///< Runs queued for the next ExecutePending().
};

}  // namespace neurus
