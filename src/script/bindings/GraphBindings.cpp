#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "editor/events/GraphEvents.hpp"
#include "script/bindings/Bindings.hpp"
#include "script/proxies/ProxyBase.hpp"

namespace py = pybind11;

namespace neurus::script {

namespace {

/// @brief Live view of the active project or built-in graph.
struct RenderGraphProxy : ProxyBase
{
	ScriptGraphView Read() const
	{
		if (view && view->ctx && view->ctx->graphProvider)
			return view->ctx->graphProvider();
		return {};
	}
};

} // namespace

void RegisterGraphBindings(py::module_& render)
{
	py::class_<RenderGraphProxy>(render, "RenderGraph")
		.def_property_readonly("uid", [](const RenderGraphProxy& graph) { return graph.Read().uid; },
		                       "The pooled graph UID, or None while using the built-in graph.")
		.def("create", [](RenderGraphProxy& graph) -> RenderGraphProxy& {
			EmitEvent(graph, RenderGraphCreateRequested{});
			if (!graph.Read().uid.has_value())
				throw std::runtime_error("No project graph was created");
			return graph;
		}, py::return_value_policy::reference,
		   "Create the editable project graph if needed and return it.");
}

void InstallGraphProxy(py::module_& render, const ScriptView* view)
{
	render.attr("graph") = py::cast(RenderGraphProxy{{0, view}});
}

} // namespace neurus::script
