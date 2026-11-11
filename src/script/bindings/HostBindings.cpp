#include <pybind11/pybind11.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "script/IScriptHost.h"
#include "script/ScriptContext.h"
#include "script/ScriptView.h"
#include "script/bindings/Bindings.h"

namespace py = pybind11;

namespace neurus
{
namespace script
{
namespace
{

IScriptHost& HostOf(const ScriptView* view)
{
	if (view == nullptr || view->ctx == nullptr || view->ctx->host == nullptr)
	{
		throw std::runtime_error("neurus: script has no application host");
	}
	return *view->ctx->host;
}

} // namespace

void RegisterHostBindings(py::module_& m)
{
	m.def_submodule("project", "Project document operations.");
	m.def_submodule("snapshot", "Snapshot capture.");
}

void InstallHostViews(py::module_& m, const ScriptView* view)
{
	py::module_ project = m.attr("project");
	py::module_ snapshot = m.attr("snapshot");

	project.def(
		"info",
		[view]()
		{
			const IScriptHost& host = HostOf(view);
			py::dict info;
			info["path"] = host.ProjectPath();
			info["dirty"] = host.IsDirty();
			return info;
		},
		"Project path and dirty state as a dict.");

	project.def(
		"save",
		[view](const std::string& path)
		{
			IScriptHost& host = HostOf(view);
			std::string error;
			if (!host.SaveProject(path, error))
				throw std::runtime_error(error.empty() ? "neurus: project save failed" : error);
			return host.ProjectPath();
		},
		py::arg("path") = std::string{},
		"Save the project to path, or the current path when omitted; returns the final path.");

	snapshot.def(
		"capture",
		[view](const std::string& path, bool all)
		{
			IScriptHost& host = HostOf(view);
			std::string error;
			std::vector<std::string> files;
			if (!host.CaptureSnapshot(path, all, error, files))
				throw std::runtime_error(error.empty() ? "neurus: snapshot capture failed" : error);

			py::list result;
			for (const auto& file : files)
				result.append(file);
			return result;
		},
		py::arg("path"), py::arg("all") = false,
		"Capture the viewport or every attachment; returns the written file paths.");
}

} // namespace script
} // namespace neurus
