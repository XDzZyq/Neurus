/**
 * @file ConfigBindings.cpp
 * @brief `neurus.data.config`: a read-only view over the run's `RenderConfig`.
 *
 * The view is constructed with the run's `ScriptView` and reads the config from
 * it on every access; each getter is `None` when the run has none. Setters never
 * write the live config; each emits a `RenderConfigChangedEvent` carrying a copy
 * with the one field changed, which the run dispatch refuses in this phase
 * (`neurus.ReadOnlyError`).
 */

#include <pybind11/pybind11.h>

#include "core/RenderConfig.h"
#include "editor/events/ConfigEvents.h"
#include "script/ScriptContext.h"
#include "script/bindings/Bindings.h"
#include "script/ProxyBase.h"

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

/// @brief `neurus.data.config`: a live render settings view for one run.
struct ConfigView : ProxyBase
{
};

}  // namespace

/// @brief Registers the `RenderConfig` view class (once).
void RegisterConfigBindings(py::module_& data)
{
	py::class_<ConfigView>(data, "RenderConfig", "Render settings; getters are None outside a run.")
		.def_property("pipeline",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return static_cast<int>(c.r_pipeline); }); },
			[](const ConfigView& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_pipeline = static_cast<RenderPipeLine>(v); }); })
		.def_property("aa",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return static_cast<int>(c.r_aa); }); },
			[](const ConfigView& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_aa = static_cast<AAAlg>(v); }); })
		.def_property("ao",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return static_cast<int>(c.r_ao); }); },
			[](const ConfigView& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_ao = static_cast<AOAlg>(v); }); })
		.def_property("shadow",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return static_cast<int>(c.r_shadow); }); },
			[](const ConfigView& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_shadow = static_cast<ShadowAlg>(v); }); })
		.def_property("ssr",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return static_cast<int>(c.r_ssr); }); },
			[](const ConfigView& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_ssr = static_cast<SSRAlg>(v); }); })
		.def_property("gamma",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_gamma; }); },
			[](const ConfigView& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_gamma = v; }); })
		.def_property("shadow_bias",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_shadow_bias; }); },
			[](const ConfigView& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_shadow_bias = v; }); })
		.def_property("ao_ksize",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_ao_ksize; }); },
			[](const ConfigView& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_ao_ksize = v; }); })
		.def_property("ao_radius",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_ao_radius; }); },
			[](const ConfigView& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_ao_radius = v; }); })
		.def_property("sampling_mode",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_sampling_mode; }); },
			[](const ConfigView& s, unsigned v) { EmitConfig(s, [v](RenderConfig& c) { c.r_sampling_mode = v; }); })
		.def_property("sample_pf",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_sample_pf; }); },
			[](const ConfigView& s, int v) { EmitConfig(s, [v](RenderConfig& c) { c.r_sample_pf = v; }); })
		.def_property("transparent",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_transparent; }); },
			[](const ConfigView& s, bool v) { EmitConfig(s, [v](RenderConfig& c) { c.r_transparent = v; }); })
		.def_property("debug_draw",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_debug_draw; }); },
			[](const ConfigView& s, bool v) { EmitConfig(s, [v](RenderConfig& c) { c.r_debug_draw = v; }); })
		.def_property("fxaa_subpix",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_fxaa_subpix; }); },
			[](const ConfigView& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_fxaa_subpix = v; }); })
		.def_property("fxaa_edge_threshold",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_fxaa_edge_threshold; }); },
			[](const ConfigView& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_fxaa_edge_threshold = v; }); })
		.def_property("fxaa_edge_threshold_min",
			[](const ConfigView& s) { return WithConfig(s, [](const RenderConfig& c) { return c.r_fxaa_edge_threshold_min; }); },
			[](const ConfigView& s, float v) { EmitConfig(s, [v](RenderConfig& c) { c.r_fxaa_edge_threshold_min = v; }); });
}

void InstallConfigViews(py::module_& data, const ScriptView* view)
{
	data.attr("config") = py::cast(ConfigView{{0, view}});
}

}  // namespace script
}  // namespace neurus
