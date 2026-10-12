/**
 * @file ConfigBindings.cpp
 * @brief `neurus.data.config`: a read-only view over the run's `RenderConfig`.
 *
 * The view is a live instance: every property reads `CurrentContext()` on
 * access, so it reflects the config of whichever run is in flight. Each
 * property is `None` outside a run (or when the run has no config), matching the
 * module-wide "empty/None rather than dereference" convention. No setter is
 * defined (write support is a later task).
 */

#include <pybind11/pybind11.h>

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

/// @brief Applies @p f to the run's config, or returns `None` when there is none.
template <typename Fn>
py::object WithConfig(Fn&& f)
{
	const ScriptContext* ctx = CurrentContext();
	if (ctx == nullptr || ctx->config == nullptr)
	{
		return py::none();
	}
	return py::cast(f(*ctx->config));
}

/// @brief `neurus.data.config`: a live, read-only `RenderConfig` view.
struct ConfigView {};

}  // namespace

void RegisterConfigBindings(py::module_& data)
{
	py::class_<ConfigView>(data, "RenderConfig",
	                       "Read-only render settings; properties are None outside a run.")
		.def_property_readonly("pipeline", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return static_cast<int>(c.r_pipeline); }); })
		.def_property_readonly("aa", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return static_cast<int>(c.r_aa); }); })
		.def_property_readonly("ao", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return static_cast<int>(c.r_ao); }); })
		.def_property_readonly("shadow", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return static_cast<int>(c.r_shadow); }); })
		.def_property_readonly("ssr", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return static_cast<int>(c.r_ssr); }); })
		.def_property_readonly("gamma", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_gamma; }); })
		.def_property_readonly("shadow_bias", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_shadow_bias; }); })
		.def_property_readonly("ao_ksize", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_ao_ksize; }); })
		.def_property_readonly("ao_radius", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_ao_radius; }); })
		.def_property_readonly("sampling_mode", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_sampling_mode; }); })
		.def_property_readonly("sample_pf", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_sample_pf; }); })
		.def_property_readonly("transparent", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_transparent; }); })
		.def_property_readonly("debug_draw", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_debug_draw; }); })
		.def_property_readonly("fxaa_subpix", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_fxaa_subpix; }); })
		.def_property_readonly("fxaa_edge_threshold", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_fxaa_edge_threshold; }); })
		.def_property_readonly("fxaa_edge_threshold_min", [](const ConfigView&)
			{ return WithConfig([](const RenderConfig& c) { return c.r_fxaa_edge_threshold_min; }); });

	data.attr("config") = py::cast(ConfigView{});
}

}  // namespace script
}  // namespace neurus
