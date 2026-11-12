/**
 * @file ScriptImports.cpp
 * @brief Import recorder, startup preload and hot reload.
 *
 * The Python half lives in a helper namespace executed into a dict held by
 * C++: it is deliberately not a module in `sys.modules`, so user code cannot
 * import or monkey-patch the recorder without reaching the private dict. The
 * finder delegates to the other `sys.meta_path` finders and wraps each spec's
 * loader, which also times `importlib.reload` - the same loader path.
 */

#include "script/ScriptImports.h"

// pybind11 first: it configures the Python headers before including them.
#include <pybind11/embed.h>

#include <Python.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/Log.h"

namespace neurus
{
namespace
{

/// Modules reported by LoadedModules(); keeps `script info` bounded.
constexpr std::size_t kMaxReportedModules = 500;

/**
 * @brief Python helper namespace installed once per interpreter.
 *
 * Records keep their insertion order (the load order); a reload updates the
 * existing entry, so the position survives. Only recorded modules whose origin
 * is under a root are candidates for reload.
 */
constexpr const char* kImportHelperSource = R"PY(import importlib
import os
import sys
import time
import traceback

_records = {}
_roots = []


def _norm(path):
    try:
        return os.path.normcase(os.path.realpath(path))
    except (OSError, ValueError):
        return ""


def _mtime_ns(origin):
    if not origin:
        return 0
    try:
        return int(os.stat(origin).st_mtime_ns)
    except (OSError, ValueError):
        return 0


def _under(path, roots):
    if not path or path in ("built-in", "frozen", "namespace"):
        return False
    return _under_real(_norm(path), roots)


def _under_real(real, roots):
    if not real:
        return False
    for root in roots:
        if real == root or real.startswith(root + os.sep):
            return True
    return False


def _record(name, origin, seconds, mtime):
    is_path = bool(origin) and origin not in ("built-in", "frozen", "namespace")
    _records[name] = {
        "origin": origin or "",
        "real": _norm(origin) if is_path else "",
        "seconds": float(seconds),
        "mtime_ns": int(mtime),
    }


class _TimingLoader:
    def __init__(self, loader):
        self._loader = loader

    def create_module(self, spec):
        create = getattr(self._loader, "create_module", None)
        if create is None:
            return None
        return create(spec)

    def exec_module(self, module):
        origin = _module_origin(module)
        mtime = _mtime_ns(origin)
        start = time.perf_counter()
        try:
            self._loader.exec_module(module)
        finally:
            _record(module.__name__, origin, time.perf_counter() - start, mtime)

    def __getattr__(self, name):
        return getattr(self._loader, name)


class _Finder:
    def __init__(self, finders):
        self._finders = list(finders)

    def find_spec(self, fullname, path, target=None):
        for finder in self._finders:
            find_spec = getattr(finder, "find_spec", None)
            if find_spec is None:
                continue
            spec = find_spec(fullname, path, target)
            if spec is None:
                continue
            loader = getattr(spec, "loader", None)
            if loader is not None and hasattr(loader, "exec_module"):
                spec.loader = _TimingLoader(loader)
            else:
                origin = getattr(spec, "origin", None) or ""
                _record(fullname, origin, 0.0, _mtime_ns(origin))
            return spec
        return None


def _install():
    existing = [finder for finder in sys.meta_path if not isinstance(finder, _Finder)]
    sys.meta_path.insert(0, _Finder(existing))


def _set_reload_roots(roots):
    _roots[:] = [_norm(root) for root in roots if _norm(root)]


def _preload(name):
    module = sys.modules.get(name)
    if module is not None:
        record = _records.get(name)
        return {
            "ok": True,
            "cached": True,
            "seconds": float(record.get("seconds", 0.0)) if record else 0.0,
            "error": "",
        }
    start = time.perf_counter()
    try:
        importlib.import_module(name)
    except BaseException:
        return {
            "ok": False,
            "cached": False,
            "seconds": time.perf_counter() - start,
            "error": traceback.format_exc(),
        }
    record = _records.get(name)
    seconds = float(record.get("seconds", 0.0)) if record else time.perf_counter() - start
    return {"ok": True, "cached": False, "seconds": seconds, "error": ""}


def _module_origin(module):
    spec = getattr(module, "__spec__", None)
    origin = getattr(spec, "origin", None) if spec is not None else None
    if not origin or origin in ("built-in", "frozen"):
        origin = getattr(module, "__file__", None) or origin or ""
    return origin or ""


def _loaded_modules(limit):
    result = []
    seen = set()
    for name, record in _records.items():
        seen.add(name)
        origin = record.get("origin", "")
        result.append({
            "name": name,
            "origin": origin,
            "seconds": float(record.get("seconds", 0.0)),
            "cached": False,
            "project": _under_real(record.get("real", ""), _roots),
        })
        if len(result) >= limit:
            return result
    for name, module in list(sys.modules.items()):
        if name in seen:
            continue
        origin = _module_origin(module)
        result.append({
            "name": name,
            "origin": origin,
            "seconds": 0.0,
            "cached": True,
            "project": _under(origin, _roots),
        })
        if len(result) >= limit:
            break
    return result


def _reload_changed(extra_roots):
    roots = list(_roots) + [_norm(root) for root in extra_roots if _norm(root)]
    reloaded = []
    for name, record in list(_records.items()):
        origin = record.get("origin", "")
        if not _under_real(record.get("real", ""), roots):
            continue
        recorded = int(record.get("mtime_ns", 0))
        if recorded <= 0:
            continue
        current = _mtime_ns(origin)
        if current <= recorded:
            continue
        module = sys.modules.get(name)
        if module is None:
            continue
        try:
            importlib.reload(module)
        except BaseException:
            return {
                "ok": False,
                "reloaded": reloaded,
                "error": "reloading module " + name + " failed:\n" + traceback.format_exc(),
            }
        entry = _records.get(name, {})
        reloaded.append({"name": name, "seconds": float(entry.get("seconds", 0.0))})
    return {"ok": True, "reloaded": reloaded, "error": ""}
)PY";

/**
 * @brief Normalizes a reload root or origin to an absolute, canonical path.
 * @return The generic-form path, or an empty string when it cannot be resolved.
 */
std::string NormalizePath(const std::string& path)
{
	if (path.empty())
	{
		return {};
	}

	std::error_code ec;
	const std::filesystem::path requested(path);
	std::filesystem::path absolute = std::filesystem::absolute(requested, ec);
	if (ec)
	{
		absolute = requested;
	}
	ec.clear();
	const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
	const std::filesystem::path resolved = ec ? absolute : canonical;
	return resolved.lexically_normal().generic_string();
}

/**
 * @brief Reads one key from a Python result dict, logging a type mismatch.
 */
template<typename T>
T ReadDict(pybind11::dict values, const char* key, const T& fallback)
{
	const pybind11::str name(key);
	if (!values.contains(name))
	{
		return fallback;
	}
	try
	{
		pybind11::object value = values[name];
		return value.cast<T>();
	}
	catch (const pybind11::cast_error& e)
	{
		NEURUS_ERR("[ScriptImports] result key '" << key
		          << "' has an unexpected type: " << e.what());
		return fallback;
	}
}

}  // namespace

struct ScriptImportRecorder::Impl
{
	bool installed = false;
	pybind11::dict   state;          ///< Private helper namespace; never in sys.modules.
	pybind11::object install;
	pybind11::object preload;
	pybind11::object loadedModules;
	pybind11::object setReloadRoots;
	pybind11::object reloadChanged;
};

ScriptImportRecorder::ScriptImportRecorder() = default;

ScriptImportRecorder::~ScriptImportRecorder() = default;

void ScriptImportRecorder::Install()
{
	if (m_impl && m_impl->installed)
	{
		return;
	}

	pybind11::gil_scoped_acquire gil;
	try
	{
		// Constructed here, not in the constructor: the engine is built before
		// the interpreter, and pybind11 members would touch it too early.
		m_impl = std::make_unique<Impl>();
		pybind11::dict scope;
		pybind11::exec(kImportHelperSource, scope, scope);
		m_impl->state = scope;
		m_impl->install = scope["_install"];
		m_impl->preload = scope["_preload"];
		m_impl->loadedModules = scope["_loaded_modules"];
		m_impl->setReloadRoots = scope["_set_reload_roots"];
		m_impl->reloadChanged = scope["_reload_changed"];
		m_impl->install();
		m_impl->installed = true;
		ApplyReloadRoots();
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[ScriptImports] failed to install the import recorder: " << e.what());
		// A half-installed helper must not be used: drop every reference so the
		// recorder reports "not installed" instead of failing per import.
		Release();
	}
}

void ScriptImportRecorder::Release()
{
	if (!m_impl)
	{
		return;
	}

	m_impl->install = pybind11::object();
	m_impl->preload = pybind11::object();
	m_impl->loadedModules = pybind11::object();
	m_impl->setReloadRoots = pybind11::object();
	m_impl->reloadChanged = pybind11::object();
	m_impl->state = pybind11::dict();
	m_impl->installed = false;
	m_impl.reset();
}

bool ScriptImportRecorder::IsInstalled() const
{
	return m_impl && m_impl->installed;
}

void ScriptImportRecorder::ApplyReloadRoots()
{
	if (!m_impl || !m_impl->installed)
	{
		return;
	}

	pybind11::list roots;
	for (const std::string& root : m_roots)
	{
		roots.append(root);
	}
	try
	{
		m_impl->setReloadRoots(roots);
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[ScriptImports] failed to apply reload roots: " << e.what());
	}
}

ScriptPreloadResult ScriptImportRecorder::Preload(const std::string& package)
{
	ScriptPreloadResult result;
	result.package = package;
	if (package.empty())
	{
		result.error = "the package name is empty";
		return result;
	}
	if (!m_impl || !m_impl->installed)
	{
		result.error = "the import recorder is not installed";
		return result;
	}

	pybind11::gil_scoped_acquire gil;
	try
	{
		pybind11::dict outcome = m_impl->preload(package).cast<pybind11::dict>();
		result.ok = ReadDict<bool>(outcome, "ok", false);
		result.cached = ReadDict<bool>(outcome, "cached", false);
		result.loadSeconds = ReadDict<double>(outcome, "seconds", 0.0);
		result.error = ReadDict<std::string>(outcome, "error", std::string{});
	}
	catch (const std::exception& e)
	{
		result.ok = false;
		result.error = e.what();
		NEURUS_ERR("[ScriptImports] preload '" << package << "' failed unexpectedly: "
		          << e.what());
	}
	return result;
}

std::vector<ScriptModuleInfo> ScriptImportRecorder::LoadedModules()
{
	std::vector<ScriptModuleInfo> modules;
	if (!m_impl || !m_impl->installed)
	{
		return modules;
	}

	pybind11::gil_scoped_acquire gil;
	try
	{
		// The helper keeps its own root copy for the project flag; reapply so a
		// root change since the last reload is reflected here.
		ApplyReloadRoots();
		pybind11::list entries =
			m_impl->loadedModules(kMaxReportedModules).cast<pybind11::list>();
		modules.reserve(static_cast<std::size_t>(entries.size()));
		for (const pybind11::handle entry : entries)
		{
			pybind11::dict values = entry.cast<pybind11::dict>();
			ScriptModuleInfo info;
			info.name = ReadDict<std::string>(values, "name", std::string{});
			info.origin = ReadDict<std::string>(values, "origin", std::string{});
			info.loadSeconds = ReadDict<double>(values, "seconds", 0.0);
			info.projectLocal = ReadDict<bool>(values, "project", false);
			info.cached = ReadDict<bool>(values, "cached", false);
			if (!info.name.empty())
			{
				modules.push_back(std::move(info));
			}
		}
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[ScriptImports] failed to enumerate loaded modules: " << e.what());
	}
	return modules;
}

void ScriptImportRecorder::SetReloadRoots(const std::vector<std::string>& roots)
{
	std::vector<std::string> normalized;
	normalized.reserve(roots.size());
	for (const std::string& root : roots)
	{
		const std::string path = NormalizePath(root);
		if (path.empty())
		{
			continue;
		}
		if (std::find(normalized.begin(), normalized.end(), path) == normalized.end())
		{
			normalized.push_back(path);
		}
	}

	m_roots = std::move(normalized);
	ApplyReloadRoots();
}

std::vector<std::string> ScriptImportRecorder::ReloadRoots() const
{
	return m_roots;
}

ReloadOutcome ScriptImportRecorder::ReloadChanged(const std::vector<std::string>& extraRoots)
{
	ReloadOutcome outcome;
	if (!m_impl || !m_impl->installed)
	{
		return outcome;
	}

	pybind11::gil_scoped_acquire gil;
	try
	{
		ApplyReloadRoots();
		pybind11::list extra;
		for (const std::string& root : extraRoots)
		{
			const std::string path = NormalizePath(root);
			if (!path.empty())
			{
				extra.append(path);
			}
		}

		pybind11::dict result = m_impl->reloadChanged(extra).cast<pybind11::dict>();
		outcome.ok = ReadDict<bool>(result, "ok", false);
		outcome.error = ReadDict<std::string>(result, "error", std::string{});
		if (result.contains(pybind11::str("reloaded")))
		{
			pybind11::list reloaded = result[pybind11::str("reloaded")].cast<pybind11::list>();
			outcome.reloaded.reserve(static_cast<std::size_t>(reloaded.size()));
			for (const pybind11::handle entry : reloaded)
			{
				pybind11::dict values = entry.cast<pybind11::dict>();
				ReloadedModule module;
				module.name = ReadDict<std::string>(values, "name", std::string{});
				module.seconds = ReadDict<double>(values, "seconds", 0.0);
				if (!module.name.empty())
				{
					outcome.reloaded.push_back(std::move(module));
				}
			}
		}
		if (!outcome.ok && outcome.error.empty())
		{
			outcome.error = "reloading a changed project module failed";
		}
	}
	catch (const std::exception& e)
	{
		outcome.ok = false;
		outcome.error = e.what();
		NEURUS_ERR("[ScriptImports] hot reload failed unexpectedly: " << e.what());
	}
	return outcome;
}

}  // namespace neurus
