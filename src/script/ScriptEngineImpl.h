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

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Log.h"
#include "editor/events/IEventQueue.h"
#include "editor/events/ScriptEvents.h"
#include "script/ScriptDispatch.h"
#include "script/ScriptEngine.h"
#include "script/ScriptSandbox.h"
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
		m_envPath = envPath.empty() ? std::string(NEURUS_PYTHON_ENV) : envPath;
		AttachEnvironment(m_envPath);

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

		// Install the in-process guardrail once, after the module exists. A
		// failure does not disable the engine: trusted runs keep working and a
		// sandboxed run is refused rather than silently running unrestricted.
		m_sandbox = std::make_unique<ScriptSandbox>();
		if (!m_sandbox->Install())
		{
			NEURUS_ERR("ScriptEngine: the script sandbox is unavailable; "
			           "sandboxed runs will be refused");
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
		// Drop the sandbox first: its helper dict holds Python objects and a
		// watchdog thread, neither of which may outlive the interpreter.
		if (m_sandbox)
		{
			m_sandbox->Reset();
			m_sandbox.reset();
		}
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

	/// @return Python runtime version, or empty before initialization.
	std::string PythonVersion() const
	{
		if (!m_ready)
			return {};
		std::string version(Py_GetVersion());
		const std::size_t newline = version.find('\n');
		if (newline != std::string::npos)
			version.resize(newline);
		return version;
	}

	/// @return Managed environment path requested at initialization.
	std::string EnvironmentPath() const
	{
		return m_envPath;
	}

	/**
	 * @brief Evaluate one expression, buffer its repr and return it.
	 *
	 * Shares the run boundary, context, policy and stream capture with
	 * RunSource so an expression with side effects is one undo entry and its
	 * prints are visible.
	 */
	std::optional<std::string> EvalStringChecked(const std::string& expression,
	                                             const ScriptContext* ctx,
	                                             const ScriptPolicy* policy)
	{
		if (!m_ready)
		{
			NEURUS_ERR("scripting not initialized");
			RecordNotReady(policy);
			return std::nullopt;
		}

		std::optional<std::string> result;
		// Declared before the GIL scope so its destructor runs after the GIL is
		// released, including when an unexpected exception skips the return.
		WatchdogJoin joinGuard(m_sandbox.get());
		{
			pybind11::gil_scoped_acquire gil;
			ContextScope scope(m_current, ctx);
			RunScope runScope(m_dispatch.EventQueue(), {});

			try
			{
				// Built while the run is still trusted: the redirect imports
				// `types`, which the policy may not allow it to read.
				StreamRedirect redirect(m_console);
				SandboxScope sandboxScope(m_sandbox.get(), policy,
				                          m_memoryLimitUnsupportedLogged);
				m_lastPolicy = PolicyOrDefault(policy);

				if (sandboxScope.ApplyFailed())
				{
					m_lastStatus = ScriptRunStatus::ScriptError;
					m_lastMessage = "The sandbox policy could not be applied.";
				}
				else
				{
					pybind11::object value =
						pybind11::eval(expression, pybind11::globals(), pybind11::globals());
					result = pybind11::repr(value).cast<std::string>();
					m_console.Append(ScriptLineKind::Result, *result + "\n");
					m_lastStatus = ScriptRunStatus::Ok;
					m_lastMessage.clear();
				}
			}
			catch (const pybind11::error_already_set& e)
			{
				// The SandboxScope destructor cleared the policy during
				// unwinding, so formatting the traceback is unrestricted.
				m_lastPolicy = PolicyOrDefault(policy);
				m_lastStatus = m_sandbox ? m_sandbox->Classify(e)
				                         : ScriptRunStatus::ScriptError;
				m_lastMessage = MessageOf(e);
				ReportError(e, "ScriptEngine::EvalString");
				result = std::nullopt;
			}
		}
		return result;
	}

	/// @brief Compatibility wrapper returning an empty string on failure.
	std::string EvalString(const std::string& expression, const ScriptContext* ctx,
	                       const ScriptPolicy* policy)
	{
		return EvalStringChecked(expression, ctx, policy).value_or(std::string{});
	}

	/// @brief Attach the queue scripted mutations are dispatched to.
	void SetEventQueue(IEventQueue* queue)
	{
		m_dispatch.SetEventQueue(queue);
	}

	/**
	 * @brief Execute a Python snippet and report whether it raised.
	 * @param code UTF-8 Python source.
	 * @param policy Restrictions for this run; null runs trusted.
	 */
	bool RunStringChecked(const std::string& code, const ScriptContext* ctx,
	                      const ScriptPolicy* policy)
	{
		if (!m_ready)
		{
			NEURUS_ERR("scripting not initialized");
			RecordNotReady(policy);
			return false;
		}

		return RunSource(code, "ScriptEngine::RunString", {}, ctx, policy);
	}

	/// @brief Execute a snippet; compatibility wrapper that ignores the result.
	void RunString(const std::string& code, const ScriptContext* ctx,
	               const ScriptPolicy* policy)
	{
		(void)RunStringChecked(code, ctx, policy);
	}

	/**
	 * @brief Read and execute a Python file, reporting whether it ran.
	 * @param path Filesystem path to a UTF-8 Python source file.
	 * @param policy Restrictions for this run; null runs trusted.
	 */
	bool RunFileChecked(const std::string& path, const ScriptContext* ctx,
	                    const ScriptPolicy* policy)
	{
		if (!m_ready)
		{
			NEURUS_ERR("scripting not initialized");
			RecordNotReady(policy);
			return false;
		}

		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			NEURUS_ERR("ScriptEngine::RunFile: cannot open '" << path << "'");
			m_lastPolicy = PolicyOrDefault(policy);
			m_lastStatus = ScriptRunStatus::ScriptError;
			m_lastMessage = "The script file could not be opened.";
			return false;
		}

		// Slurp the whole file: `py::exec` needs a complete source string, and
		// CPython decodes UTF-8 source itself.
		const std::string source((std::istreambuf_iterator<char>(file)),
		                         std::istreambuf_iterator<char>());
		return RunSource(source, "ScriptEngine::RunFile",
		                 "Run " + std::filesystem::path(path).filename().string(), ctx, policy);
	}

	/// @brief Execute a file; compatibility wrapper that ignores the result.
	void RunFile(const std::string& path, const ScriptContext* ctx, const ScriptPolicy* policy)
	{
		(void)RunFileChecked(path, ctx, policy);
	}

	/// @brief The buffer captured Python output and errors are written to.
	ScriptConsoleBuffer& Console()
	{
		return m_console;
	}

	/// @brief Queue a snippet for the next ExecutePending().
	void Submit(const std::string& code, const ScriptPolicy* policy)
	{
		PendingRun run;
		run.fromFile = false;
		run.text = code;
		if (policy != nullptr)
		{
			run.policy = *policy;
			run.hasPolicy = true;
		}
		m_pending.push_back(std::move(run));
	}

	/// @brief Queue a file for the next ExecutePending().
	void SubmitFile(const std::string& path, const ScriptPolicy* policy)
	{
		PendingRun run;
		run.fromFile = true;
		run.text = path;
		if (policy != nullptr)
		{
			run.policy = *policy;
			run.hasPolicy = true;
		}
		m_pending.push_back(std::move(run));
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
			const ScriptPolicy* policy = run.hasPolicy ? &run.policy : nullptr;
			if (run.fromFile)
			{
				RunFile(run.text, &ctx, policy);
			}
			else
			{
				RunString(run.text, &ctx, policy);
			}
		}
	}

	/// @return Outcome of the most recent run.
	ScriptRunStatus LastRunStatus() const
	{
		return m_lastStatus;
	}

	/// @return Human-readable detail for the last run, empty on success.
	std::string LastRunMessage() const
	{
		return m_lastMessage;
	}

	/// @return The policy the last run executed under.
	ScriptPolicy LastPolicy() const
	{
		return m_lastPolicy;
	}

private:
	/// @brief Shared run path: GIL, stdout/stderr capture, sandbox and errors.
	/// @param source Python source to execute.
	/// @param entryName Caller name used in error reporting, not as a label.
	/// @param runName History label for the run; empty means "Run Script".
	/// @param ctx Document view to publish for the run.
	/// @param policy Restrictions for this run; null runs trusted.
	bool RunSource(const std::string& source, const char* entryName, const std::string& runName,
	               const ScriptContext* ctx, const ScriptPolicy* policy)
	{
		bool ok = false;
		// Declared before the GIL scope so its destructor runs after the GIL is
		// released, including when an unexpected exception skips the return.
		WatchdogJoin joinGuard(m_sandbox.get());
		{
			pybind11::gil_scoped_acquire gil;

			// Publish this run's document view. The scope republishes
			// `m_current` for the run and clears it on every exit path, so no
			// Python object can read a finished run's state.
			ContextScope scope(m_current, ctx);

			// Bracket the run so the operation sink compacts every write it
			// makes into one undo step. Declared before the exec try-block and
			// after the document is published, so it closes on every exit path -
			// including a raising script - and the run's writes are recorded
			// while the context is still live.
			RunScope runScope(m_dispatch.EventQueue(), runName);

			try
			{
				// Redirect before running so `print` and tracebacks land in the
				// buffer, and before the policy activates: installing it imports
				// `types`, which the policy may not allow.
				StreamRedirect redirect(m_console);
				SandboxScope sandboxScope(m_sandbox.get(), policy,
				                          m_memoryLimitUnsupportedLogged);

				m_lastPolicy = PolicyOrDefault(policy);
				if (sandboxScope.ApplyFailed())
				{
					m_lastStatus = ScriptRunStatus::ScriptError;
					m_lastMessage = "The sandbox policy could not be applied.";
				}
				else
				{
					pybind11::exec(source, pybind11::globals(), pybind11::globals());
					ok = true;
					m_lastStatus = ScriptRunStatus::Ok;
					m_lastMessage.clear();
				}
			}
			catch (const pybind11::error_already_set& e)
			{
				// The SandboxScope destructor cleared the policy during
				// unwinding, so formatting the traceback is unrestricted.
				m_lastPolicy = PolicyOrDefault(policy);
				m_lastStatus = m_sandbox ? m_sandbox->Classify(e)
				                         : ScriptRunStatus::ScriptError;
				m_lastMessage = MessageOf(e);
				ReportError(e, entryName);
			}
		}
		return ok;
	}

	/// @brief The policy a run executed under, for the status accessors.
	static ScriptPolicy PolicyOrDefault(const ScriptPolicy* policy)
	{
		return policy != nullptr ? *policy : ScriptPolicy::Trusted();
	}

	/// @brief Records the "no interpreter" outcome for a rejected run.
	void RecordNotReady(const ScriptPolicy* policy)
	{
		m_lastPolicy = PolicyOrDefault(policy);
		m_lastStatus = ScriptRunStatus::NotReady;
		m_lastMessage = "Scripting is not initialized.";
	}

	/// @brief Extracts the exception's own message, without the traceback.
	static std::string MessageOf(const pybind11::error_already_set& error)
	{
		try
		{
			return pybind11::str(error.value()).cast<std::string>();
		}
		catch (const std::exception& formatError)
		{
			NEURUS_ERR("ScriptEngine: exception message formatting failed: "
			           << formatError.what());
			return error.what();
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
	 * @brief Applies one run's policy and stops it on every exit path.
	 *
	 * Constructed with the GIL held and after the stream redirect, destroyed
	 * during unwinding or at the end of the try block - always before the GIL
	 * guard is released, which is what lets `StopWatchdog()` poll a pending
	 * injection safely. It never joins the watchdog: `RunSource` and
	 * `EvalStringChecked` do that after releasing the GIL.
	 */
	class SandboxScope
	{
	public:
		SandboxScope(ScriptSandbox* sandbox, const ScriptPolicy* policy,
		             bool& memoryLimitUnsupportedLogged)
			: m_sandbox(sandbox)
		{
			if (m_sandbox == nullptr)
			{
				// No helper means no enforceable restriction. Trusted still
				// runs; a sandboxed request is refused rather than downgraded.
				m_applyFailed = policy != nullptr && policy->mode == ScriptMode::Sandboxed;
				return;
			}

			m_sandbox->ClearPolicy();
			if (policy == nullptr || policy->mode != ScriptMode::Sandboxed)
			{
				return;
			}

			// Before the policy activates: the probe must not be judged by it.
			if (policy->memoryLimitBytes > 0
			    && !m_sandbox->ApplyMemoryLimit(policy->memoryLimitBytes)
			    && !memoryLimitUnsupportedLogged)
			{
				NEURUS_ERR("ScriptEngine: in-process memory limiting is unavailable "
				           "on this platform; the run continues unbounded");
				memoryLimitUnsupportedLogged = true;
			}

			if (!m_sandbox->SetPolicy(*policy))
			{
				m_applyFailed = true;
				return;
			}

			m_sandbox->BeginWatchdog(policy->wallClockLimit, PyThreadState_Get());
		}

		~SandboxScope()
		{
			if (m_sandbox == nullptr)
			{
				return;
			}
			// Order matters: the watchdog must not inject while the policy is
			// still clearing, and the memory limit is restored last.
			m_sandbox->StopWatchdog();
			m_sandbox->ClearPolicy();
			m_sandbox->RestoreMemoryLimit();
		}

		SandboxScope(const SandboxScope&) = delete;
		SandboxScope& operator=(const SandboxScope&) = delete;

		/// @return True when a sandboxed policy could not be applied.
		bool ApplyFailed() const
		{
			return m_applyFailed;
		}

	private:
		ScriptSandbox* m_sandbox = nullptr;
		bool           m_applyFailed = false;
	};

	/**
	 * @brief Joins the watchdog after the enclosing GIL scope has unwound.
	 *
	 * Declared before the GIL guard of a run, so destruction order releases the
	 * GIL first and a blocking join cannot deadlock against the watchdog's
	 * `PyGILState_Ensure`.
	 */
	class WatchdogJoin
	{
	public:
		explicit WatchdogJoin(ScriptSandbox* sandbox)
			: m_sandbox(sandbox)
		{
		}

		~WatchdogJoin()
		{
			if (m_sandbox != nullptr)
			{
				m_sandbox->JoinWatchdog();
			}
		}

		WatchdogJoin(const WatchdogJoin&) = delete;
		WatchdogJoin& operator=(const WatchdogJoin&) = delete;

	private:
		ScriptSandbox* m_sandbox = nullptr;
	};

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
	 * @brief Brackets one script run so its writes compact into one undo step.
	 *
	 * Emits `ScriptRunStarted` on construction and `ScriptRunFinished` on
	 * destruction; `ScriptController` forwards both to the operation sink's
	 * script scope. The history label travels with `ScriptRunStarted`, so a
	 * console run records as "Run Script" and a file run as "Run <file>.py". A
	 * raising run still reaches the destructor, so the scope always closes.
	 *
	 * Constructor and destructor are both `noexcept`: an exception leaving a
	 * destructor during unwinding calls `std::terminate`. Clearing
	 * @ref m_events in the constructor's catch keeps the pair symmetric - if
	 * Started never landed, Finished must not be sent either.
	 */
	class RunScope
	{
	public:
		explicit RunScope(IEventQueue* events, const std::string& name) noexcept
			: m_events(events)
		{
			try
			{
				if (m_events != nullptr)
				{
					m_events->Emit(ScriptRunStarted{ name });
				}
			}
			catch (...)
			{
				// Clear the pointer so Started and Finished stay paired: a run
				// whose start never landed must not send its finish either. The
				// run still executes; it just records per-edit instead of as one
				// undo step.
				NEURUS_ERR("[ScriptEngine] ScriptRunStarted could not be emitted: "
				           "this run will record individual undo steps");
				m_events = nullptr;
			}
		}

		~RunScope() noexcept
		{
			try
			{
				if (m_events != nullptr)
				{
					m_events->Emit(ScriptRunFinished{});
				}
			}
			catch (...)
			{
				NEURUS_ERR("[ScriptEngine] ScriptRunFinished could not be emitted");
			}
		}

		RunScope(const RunScope&) = delete;
		RunScope& operator=(const RunScope&) = delete;

	private:
		IEventQueue* m_events = nullptr;
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
		bool         fromFile = false;  ///< true = @ref text is a path; else source.
		bool         hasPolicy = false; ///< true = @ref policy applies; else trusted.
		std::string  text;
		ScriptPolicy policy;            ///< Copied at submission time.
	};

	bool m_ready = false;
	std::string m_envPath;              ///< Environment requested at initialization.
	pybind11::module_ m_neuru;           ///< Embedded module; views registered once.
	ScriptConsoleBuffer m_console;
	ScriptDispatch m_dispatch;          ///< Funnel for scripted mutations; attached by SetEventQueue().
	ScriptContext m_current;            ///< Document published for the run in progress.
	script::ScriptView m_view{&m_current, &m_dispatch};  ///< Injected into the module once.
	std::vector<PendingRun> m_pending;  ///< Runs queued for the next ExecutePending().
	std::unique_ptr<ScriptSandbox> m_sandbox;  ///< Guardrail helper; installed at Init().
	ScriptRunStatus m_lastStatus = ScriptRunStatus::NotReady;  ///< Last run outcome.
	std::string m_lastMessage;          ///< Last run's message, empty on success.
	ScriptPolicy m_lastPolicy;          ///< Policy the last run executed under.
	bool m_memoryLimitUnsupportedLogged = false;  ///< Log the memory-limit warning once.
};

}  // namespace neurus
