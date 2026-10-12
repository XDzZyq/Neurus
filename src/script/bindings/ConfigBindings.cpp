/**
 * @file ConfigBindings.cpp
 * @brief `neurus.data.config`: a read-only view over the run's `RenderConfig`.
 *
 * The view is a live instance: every property reads `CurrentContext()` on
 * access, so it reflects the config of whichever run is in flight. No setter is
 * defined (write support is a later task).
 */

#include <pybind11/pybind11.h>

#include <stdexcept>

#include "core/RenderConfig.h"
#include "script/bindings/Bindings.h"
#include "script/bindings/Runtime.h"

namespace py = pybind11;

namespace neurus
{
namespace script
{
namespace
{

/// @brief The run's render config, or raises a clear error when absent.
const RenderConfig& RequireConfig()
{
	const ScriptContext* ctx = CurrentContext();
	if (ctx == nullptr || ctx->config == nullptr)
	{
		throw std::runtime_error("neurus: no render config in this run");
	}
	return *ctx->config;
}

/// @brief `neurus.data.config`: a live, read-only `RenderConfig` view.
struct ConfigView {};

}  // namespace

void RegisterConfigBindings(py::module_& data)
{
	py::class_<ConfigView>(data, "RenderConfig", "Read-only view of the render settings.")
		.def_property_readonly("pipeline",
		     [](const ConfigView&) { return static_cast<int>(RequireConfig().r_pipeline); })
		.def_property_readonly("aa",
		     [](const ConfigView&) { return static_cast<int>(RequireConfig().r_aa); })
		.def_property_readonly("ao",
		     [](const ConfigView&) { return static_cast<int>(RequireConfig().r_ao); })
		.def_property_readonly("shadow",
		     [](const ConfigView&) { return static_cast<int>(RequireConfig().r_shadow); })
		.def_property_readonly("ssr",
		     [](const ConfigView&) { return static_cast<int>(RequireConfig().r_ssr); })
		.def_property_readonly("gamma", [](const ConfigView&) { return RequireConfig().r_gamma; })
		.def_property_readonly("shadow_bias",
		     [](const ConfigView&) { return RequireConfig().r_shadow_bias; })
		.def_property_readonly("ao_ksize", [](const ConfigView&) { return RequireConfig().r_ao_ksize; })
		.def_property_readonly("ao_radius", [](const ConfigView&) { return RequireConfig().r_ao_radius; });

	data.attr("config") = py::cast(ConfigView{});
}

}  // namespace script
}  // namespace neurus
