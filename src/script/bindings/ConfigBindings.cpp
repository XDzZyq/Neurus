/**
 * @file ConfigBindings.cpp
 * @brief `neurus.render.config`: a read-only proxy over the run's `RenderConfig`.
 *
 * The proxy is constructed with the run's `ScriptView` and reads the config from
 * it on every access; each getter is `None` when the run has none. Setters never
 * write the live config directly; each emits a `RenderConfigChangedEvent`
 * carrying a copy with the one field changed, which the render config controller
 * applies synchronously.
 */

#include <pybind11/pybind11.h>

#include "core/RenderConfig.h"
#include "editor/events/ConfigEvents.h"
#include "script/ScriptContext.h"
#include "script/bindings/Bindings.h"
#include "script/proxies/ProxyBase.h"

namespace py = pybind11;

namespace neurus
{
namespace script
{
namespace
{

/// @brief Applies @p f to the run's config, or returns `None` when there is none.
template <typename Fn>
py::object WithConfig(const ProxyBase& self, Fn&& f)
{
	const RenderConfig* config =
	    (self.view != nullptr && self.view->ctx != nullptr) ? self.view->ctx->config : nullptr;
	if (config == nullptr)
	{
		return py::none();
	}
	return py::cast(f(*config));
}

/// @brief Emits a change carrying a copy of the run's config, mutated by @p mutate.
template <typename Mutator>
void EmitConfig(const ProxyBase& self, Mutator&& mutate)
{
	const RenderConfig* live =
	    (self.view != nullptr && self.view->ctx != nullptr) ? self.view->ctx->config : nullptr;
	RenderConfig config = live != nullptr ? *live : RenderConfig{};
	mutate(config);
	EmitEvent(self, RenderConfigChangedEvent{config});
}

/// @brief `neurus.render.config`: a live render settings proxy for one run.
struct ConfigProxy : ProxyBase
{
};

}  // namespace

/// @brief Registers the `RenderConfig` proxy class (once) on the `render` module.
void RegisterConfigBindings(py::module_& render)
{
	py::class_<ConfigProxy>(render, "RenderConfig", "Render settings; getters are None outside a run.")
		.def_property("pipeline",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return static_cast<int>(c.r_pipeline); }); },
			[](const ConfigProxy& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_pipeline = static_cast<RenderPipeLine>(v); }); })
		.def_property("aa",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return static_cast<int>(c.r_aa); }); },
			[](const ConfigProxy& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_aa = static_cast<AAAlg>(v); }); })
		.def_property("ao",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return static_cast<int>(c.r_ao); }); },
			[](const ConfigProxy& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_ao = static_cast<AOAlg>(v); }); })
		.def_property("shadow",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return static_cast<int>(c.r_shadow); }); },
			[](const ConfigProxy& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_shadow = static_cast<ShadowAlg>(v); }); })
		.def_property("ssr",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return static_cast<int>(c.r_ssr); }); },
			[](const ConfigProxy& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_ssr = static_cast<SSRAlg>(v); }); })
		.def_property("gamma",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_gamma; }); },
			[](const ConfigProxy& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_gamma = v; }); })
		.def_property("shadow_bias",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_shadow_bias; }); },
			[](const ConfigProxy& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_shadow_bias = v; }); })
		.def_property("ao_ksize",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_ao_ksize; }); },
			[](const ConfigProxy& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_ao_ksize = v; }); })
		.def_property("ao_radius",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_ao_radius; }); },
			[](const ConfigProxy& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_ao_radius = v; }); })
		.def_property("sampling_mode",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_sampling_mode; }); },
			[](const ConfigProxy& s, unsigned v) { EmitConfig(s, [v](RenderConfig& c) { c.r_sampling_mode = v; }); })
		.def_property("sample_pf",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_sample_pf; }); },
			[](const ConfigProxy& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_sample_pf = v; }); })
		.def_property("transparent",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_transparent; }); },
			[](const ConfigProxy& s, bool v) { EmitConfig(s, [v](RenderConfig& c) { c.r_transparent = v; }); })
		.def_property("debug_draw",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_debug_draw; }); },
			[](const ConfigProxy& s, bool v) { EmitConfig(s, [v](RenderConfig& c) { c.r_debug_draw = v; }); })
		.def_property("fxaa_subpix",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_fxaa_subpix; }); },
			[](const ConfigProxy& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_fxaa_subpix = v; }); })
		.def_property("fxaa_edge_threshold",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_fxaa_edge_threshold; }); },
			[](const ConfigProxy& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_fxaa_edge_threshold = v; }); })
		.def_property("fxaa_edge_threshold_min",
			[](const ConfigProxy& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_fxaa_edge_threshold_min; }); },
			[](const ConfigProxy& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_fxaa_edge_threshold_min = v; }); });
}

/// @brief Installs the live config proxy as `neurus.render.config`.
void InstallConfigProxies(py::module_& render, const ScriptView* view)
{
	render.attr("config") = py::cast(ConfigProxy{{0, view}});
}

}  // namespace script
}  // namespace neurus
