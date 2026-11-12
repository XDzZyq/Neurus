/**
 * @file ScriptSandbox.cpp
 * @brief Python-side policy hook, import gate and wall-clock watchdog.
 */

#include "script/ScriptSandbox.h"

#include <pybind11/stl.h>

#include "core/Log.h"

#include <exception>

namespace neurus
{
namespace
{

/**
 * @brief Private helper source executed once into its own dict.
 *
 * Nothing here is inserted into `sys.modules`: the dict is held by
 * `ScriptSandbox`, so a script cannot import the enforcement code and cannot
 * overwrite it by name. The policy object stays reachable through the hook's
 * closure, and `set_policy` is only called from C++.
 */
constexpr const char* kHelperSource = R"PY(
import builtins
import os
import sys

class _SandboxViolation(PermissionError):
    """An operation was refused by the active script policy."""

class _SandboxImportViolation(ImportError):
    """A module import was refused by the active script policy."""

class _SandboxTimeout(Exception):
    def __init__(self, message="script exceeded its wall-clock limit"):
        super().__init__(message)

class _Policy:
    __slots__ = ("read_roots", "write_roots", "blocked_modules", "allow_network",
                 "allow_subprocess", "allow_ctypes")

    def __init__(self, read_roots, write_roots, blocked_modules, allow_network,
                 allow_subprocess, allow_ctypes):
        self.read_roots = read_roots
        self.write_roots = write_roots
        self.blocked_modules = blocked_modules
        self.allow_network = allow_network
        self.allow_subprocess = allow_subprocess
        self.allow_ctypes = allow_ctypes

_policy = None
_saved_limits = None
_original_import = builtins.__import__

_READ_EVENTS = frozenset(("os.listdir", "os.scandir", "os.chdir"))
_WRITE_EVENTS = frozenset(("os.mkdir", "os.rmdir", "os.remove", "os.unlink",
                           "os.chmod", "os.chown", "os.utime", "os.truncate",
                           "os.mkfifo", "os.mknod", "os.chflags", "os.lchflags"))
_WRITE_DIR_FD_INDEX = {
    "os.mkdir": 2,
    "os.rmdir": 1,
    "os.remove": 1,
    "os.unlink": 1,
    "os.chmod": 2,
    "os.chown": 3,
    "os.utime": 3,
    "os.mkfifo": 2,
    "os.mknod": 3,
}
_TWO_PATH_EVENTS = frozenset(("os.rename", "os.replace", "os.link", "os.symlink"))
_SUBPROCESS_EVENTS = frozenset(("subprocess.Popen", "os.system", "os.exec",
                                "os.spawn", "os.posix_spawn", "os.fork",
                                "os.forkpty", "pty.spawn", "os.startfile"))
_NETWORK_EVENTS = frozenset(("socket.connect", "socket.bind", "socket.sendto",
                             "socket.getaddrinfo", "socket.gethostbyname",
                             "socket.gethostbyname_ex", "socket.gethostbyaddr",
                             "socket.sethostname", "socket.getnameinfo"))

def _normalize(path):
    try:
        text = os.fsdecode(path)
        return os.path.normcase(os.path.realpath(text))
    except Exception:
        # An unnormalizable path cannot be proven to be inside a root.
        return None

def _under(path, roots):
    normalized = _normalize(path)
    if normalized is None:
        return False
    for root in roots:
        try:
            if os.path.commonpath([normalized, root]) == root:
                return True
        except (TypeError, ValueError):
            # Another drive, or an absolute/relative mix: not proven inside.
            continue
    return False

# The interpreter's own installation stays readable: a script cannot run at all
# if importing a stdlib module is a read violation. This widens reads only,
# never writes or the permission flags.
def _library_roots():
    roots = []
    candidates = [sys.base_prefix, sys.prefix, sys.base_exec_prefix, sys.exec_prefix]
    try:
        candidates.extend(sys.path)
    except Exception:
        pass
    for candidate in candidates:
        if not isinstance(candidate, str) or not candidate:
            continue
        normalized = _normalize(candidate)
        if normalized is not None and normalized not in roots:
            roots.append(normalized)
    return tuple(roots)

_LIBRARY_ROOTS = _library_roots()

def _check_read(policy, path):
    if isinstance(path, int):
        return
    if path is None:
        path = os.getcwd()
    if not _under(path, policy.read_roots) and not _under(path, _LIBRARY_ROOTS):
        raise _SandboxViolation(
            "sandbox: read outside allowed roots (rule: read_roots)")

def _check_write(policy, path):
    if isinstance(path, int):
        return
    if path is None:
        path = os.getcwd()
    if not _under(path, policy.write_roots):
        raise _SandboxViolation(
            "sandbox: write outside allowed roots (rule: write_roots)")

def _open_is_write(mode, flags):
    if isinstance(mode, str):
        return any(marker in mode for marker in "wax+")
    if isinstance(flags, int):
        write_flags = os.O_WRONLY | os.O_RDWR | os.O_APPEND | os.O_CREAT | os.O_TRUNC
        return bool(flags & write_flags)
    return False

def _check_open(policy, path, mode, flags):
    if isinstance(path, int):
        return
    if _open_is_write(mode, flags):
        _check_write(policy, path)
    else:
        _check_read(policy, path)

def _check_blocked_import(name):
    policy = _policy
    if policy is None or not policy.blocked_modules:
        return
    root = name.split(".", 1)[0]
    if name in policy.blocked_modules or root in policy.blocked_modules:
        raise _SandboxImportViolation(
            "sandbox: module '%s' is blocked by policy (rule: blocked_modules)" % name)

class _BlockedModuleFinder:
    def find_spec(self, fullname, path=None, target=None):
        _check_blocked_import(fullname)
        return None

def _guarded_import(name, globals=None, locals=None, fromlist=(), level=0):
    if level == 0:
        _check_blocked_import(name)
    return _original_import(name, globals, locals, fromlist, level)

def set_policy(read_roots=None, write_roots=None, blocked_modules=None,
               allow_network=True, allow_subprocess=True, allow_ctypes=True):
    global _policy
    if read_roots is None:
        _policy = None
        return

    def normalize_roots(values):
        roots = []
        for value in values or ():
            normalized = _normalize(value)
            if normalized is not None:
                roots.append(normalized)
        return tuple(roots)

    _policy = _Policy(
        normalize_roots(read_roots),
        normalize_roots(write_roots),
        tuple(str(module) for module in (blocked_modules or ())),
        bool(allow_network),
        bool(allow_subprocess),
        bool(allow_ctypes),
    )

def clear_policy():
    global _policy
    _policy = None

def apply_memory_limit(limit_bytes):
    global _saved_limits
    if not limit_bytes:
        return True
    try:
        import resource
    except ImportError:
        # No in-process address-space limit on this platform. The caller logs
        # once and the run continues unbounded.
        return False
    try:
        current = resource.getrlimit(resource.RLIMIT_AS)
        soft = int(limit_bytes)
        if current[0] != resource.RLIM_INFINITY and current[0] < soft:
            soft = current[0]
        resource.setrlimit(resource.RLIMIT_AS, (soft, current[1]))
        _saved_limits = current
        return True
    except Exception:
        return False

def restore_memory_limit():
    global _saved_limits
    if _saved_limits is None:
        return True
    saved = _saved_limits
    _saved_limits = None
    try:
        import resource
        resource.setrlimit(resource.RLIMIT_AS, saved)
        return True
    except Exception:
        return False

def _is_subprocess_event(event):
    return (event in _SUBPROCESS_EVENTS or event.startswith("os.exec")
            or event.startswith("os.spawn"))

def _is_network_event(event):
    return event in _NETWORK_EVENTS

def _check_dir_fd(args, index):
    # A relative path resolved against a dir_fd cannot be proven inside a root,
    # so a call that names one is refused rather than guessed.
    if index < 0 or index >= len(args):
        return
    if args[index] is not None:
        raise _SandboxViolation(
            "sandbox: dir_fd-relative path cannot be proven inside the roots "
            "(rule: write_roots)")

def _audit_policy(policy, event, args):
    if event == "open":
        if len(args) >= 2:
            _check_open(policy, args[0], args[1], args[2] if len(args) > 2 else 0)
        return

    if event in _READ_EVENTS:
        _check_read(policy, args[0] if args else None)
        return

    if event in _WRITE_EVENTS:
        _check_dir_fd(args, _WRITE_DIR_FD_INDEX.get(event, -1))
        _check_write(policy, args[0] if args else None)
        return

    if event in _TWO_PATH_EVENTS:
        _check_dir_fd(args, 2)
        _check_dir_fd(args, 3)
        if len(args) >= 2:
            _check_write(policy, args[0])
            _check_write(policy, args[1])
        return

    if _is_subprocess_event(event):
        if not policy.allow_subprocess:
            raise _SandboxViolation(
                "sandbox: subprocess creation blocked by policy "
                "(rule: allow_subprocess)")
        return

    if _is_network_event(event):
        if not policy.allow_network:
            raise _SandboxViolation(
                "sandbox: network access blocked by policy (rule: allow_network)")
        return

    if event.startswith("ctypes."):
        if not policy.allow_ctypes:
            raise _SandboxViolation(
                "sandbox: ctypes access blocked by policy (rule: allow_ctypes)")

def _audit(event, args):
    policy = _policy
    if policy is None:
        return
    try:
        _audit_policy(policy, event, args)
    except (_SandboxViolation, _SandboxImportViolation, _SandboxTimeout):
        raise
    except Exception as error:
        # Fail closed: an unexpected payload must never widen the policy.
        raise _SandboxViolation(
            "sandbox: policy check failed (rule: internal)") from error

if not getattr(sys, "_neurus_sandbox_hook_installed", False):
    sys.addaudithook(_audit)
    sys._neurus_sandbox_hook_installed = True

sys.meta_path.insert(0, _BlockedModuleFinder())
builtins.__import__ = _guarded_import

_neurus_module._SandboxViolation = _SandboxViolation
_neurus_module._SandboxImportViolation = _SandboxImportViolation
_neurus_module._SandboxTimeout = _SandboxTimeout
)PY";

}  // namespace

ScriptSandbox::~ScriptSandbox()
{
	// The GIL is held on the engine's main thread; a teardown that skipped
	// Reset() still releases the Python objects here.
	Reset();
}

bool ScriptSandbox::Install()
{
	if (m_installed)
	{
		return true;
	}

	try
	{
		pybind11::dict helper;
		helper["_neurus_module"] = pybind11::module_::import("neurus");
		pybind11::exec(kHelperSource, helper, helper);
		m_helper = helper;
		m_violation = helper["_SandboxViolation"];
		m_importViolation = helper["_SandboxImportViolation"];
		m_timeoutObject = helper["_SandboxTimeout"];
		m_installed = true;
		return true;
	}
	catch (const pybind11::error_already_set& error)
	{
		NEURUS_ERR("ScriptSandbox: helper installation failed: " << error.what());
		return false;
	}
}

bool ScriptSandbox::SetPolicy(const ScriptPolicy& policy)
{
	if (!m_installed)
	{
		NEURUS_ERR("ScriptSandbox: cannot apply a policy before Install()");
		return false;
	}

	try
	{
		pybind11::object setPolicy = m_helper["set_policy"];
		setPolicy(policy.readRoots, policy.writeRoots, policy.blockedModules,
		          policy.allowNetwork, policy.allowSubprocess, policy.allowCtypes);
		return true;
	}
	catch (const pybind11::error_already_set& error)
	{
		NEURUS_ERR("ScriptSandbox: applying the policy failed: " << error.what());
		ClearPolicy();
		return false;
	}
}

void ScriptSandbox::ClearPolicy()
{
	if (!m_installed)
	{
		return;
	}

	try
	{
		pybind11::object clear = m_helper["clear_policy"];
		clear();
	}
	catch (const pybind11::error_already_set& error)
	{
		NEURUS_ERR("ScriptSandbox: clearing the policy failed: " << error.what());
	}
}

ScriptRunStatus ScriptSandbox::Classify(const pybind11::error_already_set& error) const
{
	try
	{
		if (m_timeoutObject && error.matches(m_timeoutObject))
		{
			return ScriptRunStatus::Timeout;
		}
		if ((m_violation && error.matches(m_violation))
		    || (m_importViolation && error.matches(m_importViolation)))
		{
			return ScriptRunStatus::SandboxViolation;
		}
	}
	catch (const pybind11::error_already_set& matchError)
	{
		NEURUS_ERR("ScriptSandbox: exception classification failed: " << matchError.what());
	}
	return ScriptRunStatus::ScriptError;
}

void ScriptSandbox::BeginWatchdog(std::chrono::milliseconds limit, PyThreadState* target)
{
	if (limit.count() <= 0 || target == nullptr || !m_timeoutObject)
	{
		return;
	}

	if (m_watchdog.joinable())
	{
		NEURUS_ERR("ScriptSandbox: a previous watchdog was not joined; stopping it first");
		StopWatchdog();
		m_watchdog.join();
	}

	// CPython 3.12's PyThreadState_GetID is not the id SetAsyncExc matches on
	// Windows: it can be a sequential state id (often 1), while the async
	// injection matches the OS thread id. Prefer the thread id and keep the
	// state id only as a fallback for an unusual embedding.
	const unsigned long threadId = PyThread_get_thread_ident();
	const unsigned long stateId = static_cast<unsigned long>(PyThreadState_GetID(target));
	const unsigned long targetId = threadId != 0 ? threadId : stateId;
	if (targetId == 0)
	{
		NEURUS_ERR("ScriptSandbox: thread state has no id; the run has no watchdog");
		return;
	}

	{
		std::lock_guard<std::mutex> lock(m_watchdogMutex);
		m_watchdogStop = false;
		m_watchdogTargetId = targetId;
		m_watchdogInjectedId = 0;
		m_watchdogDeadline = std::chrono::steady_clock::now() + limit;
	}

	try
	{
		m_watchdog = std::thread([this]() { WatchdogLoop(); });
	}
	catch (const std::exception& error)
	{
		// A missing watchdog must not fail the run; it just has no time limit.
		NEURUS_ERR("ScriptSandbox: watchdog thread could not start: " << error.what());
		std::lock_guard<std::mutex> lock(m_watchdogMutex);
		m_watchdogStop = true;
		m_watchdogTargetId = 0;
	}
}

void ScriptSandbox::StopWatchdog()
{
	unsigned long injected = 0;
	{
		std::lock_guard<std::mutex> lock(m_watchdogMutex);
		m_watchdogStop = true;
		m_watchdogTargetId = 0;
		injected = m_watchdogInjectedId;
		m_watchdogInjectedId = 0;
	}
	m_watchdogCv.notify_all();

	if (injected != 0 && Py_IsInitialized())
	{
		// A timeout that landed after the run finished must not interrupt the
		// next run: clearing with a null exception drops the pending request.
		(void)PyThreadState_SetAsyncExc(injected, nullptr);
	}
}

void ScriptSandbox::JoinWatchdog()
{
	if (m_watchdog.joinable())
	{
		m_watchdog.join();
	}
}

bool ScriptSandbox::ApplyMemoryLimit(std::uint64_t bytes)
{
	if (!m_installed)
	{
		return false;
	}
	if (bytes == 0)
	{
		return true;
	}

	try
	{
		pybind11::object apply = m_helper["apply_memory_limit"];
		return apply(bytes).cast<bool>();
	}
	catch (const pybind11::error_already_set& error)
	{
		NEURUS_ERR("ScriptSandbox: applying the memory limit failed: " << error.what());
		return false;
	}
}

void ScriptSandbox::RestoreMemoryLimit()
{
	if (!m_installed)
	{
		return;
	}

	try
	{
		pybind11::object restore = m_helper["restore_memory_limit"];
		if (!restore().cast<bool>())
		{
			NEURUS_ERR("ScriptSandbox: the previous memory limit could not be restored");
		}
	}
	catch (const pybind11::error_already_set& error)
	{
		NEURUS_ERR("ScriptSandbox: restoring the memory limit failed: " << error.what());
	}
}

void ScriptSandbox::Reset()
{
	StopWatchdog();
	JoinWatchdog();

	// Order matters only in that every object is released with the GIL held.
	m_timeoutObject = pybind11::object();
	m_importViolation = pybind11::object();
	m_violation = pybind11::object();
	m_helper = pybind11::dict();
	m_installed = false;
}

void ScriptSandbox::WatchdogLoop()
{
	try
	{
		unsigned long targetId = 0;
		{
			std::unique_lock<std::mutex> lock(m_watchdogMutex);
			if (m_watchdogCv.wait_until(lock, m_watchdogDeadline,
			                            [this]() { return m_watchdogStop; }))
			{
				return;
			}
			targetId = m_watchdogTargetId;
		}

		if (targetId == 0)
		{
			return;
		}

		const PyGILState_STATE gil = PyGILState_Ensure();
		{
			std::lock_guard<std::mutex> lock(m_watchdogMutex);
			// Re-check under the GIL: a StopWatchdog that ran while this thread
			// was waiting for the GIL set the flag before releasing it.
			if (m_watchdogStop || m_watchdogTargetId == 0)
			{
				targetId = 0;
			}
		}
		if (targetId != 0)
		{
			const int raised = PyThreadState_SetAsyncExc(targetId, m_timeoutObject.ptr());
			if (raised > 0)
			{
				std::lock_guard<std::mutex> lock(m_watchdogMutex);
				m_watchdogInjectedId = targetId;
			}
		}
		PyGILState_Release(gil);
	}
	catch (const std::exception& error)
	{
		// Nothing may escape a thread entry point.
		NEURUS_ERR("ScriptSandbox: watchdog failed: " << error.what());
	}
	catch (...)
	{
		NEURUS_ERR("ScriptSandbox: watchdog failed with an unknown exception");
	}
}

}  // namespace neurus
