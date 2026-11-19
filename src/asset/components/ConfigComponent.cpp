#include "asset/components/ConfigComponent.hpp"
#include "core/RenderConfig.hpp"
#include "core/Log.hpp"
#include "core/Serialize.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

namespace neurus::project
{

ConfigComponent::ConfigComponent(std::shared_ptr<RenderConfig>& config) : m_config(&config) {}

void ConfigComponent::Save(cereal::JSONOutputArchive& ar) const
{
	if (*m_config)
		ar(cereal::make_nvp("proj_config", **m_config));
}

void ConfigComponent::Load(cereal::JSONInputArchive& ar)
{
	m_config->reset();
	auto loaded = std::make_shared<RenderConfig>();
	try
	{
		if (archive::LoadFresh(ar, "proj_config", *loaded))
			*m_config = std::move(loaded);
	}
	catch (const cereal::Exception& error)
	{
		NEURUS_LOG("ConfigComponent::Load: " << error.what() << " - using implicit defaults.");
	}
}

void ConfigComponent::LoadJson(const std::string& object)
{
	m_config->reset();
	try
	{
		auto document = nlohmann::ordered_json::parse(object);
		auto config = document.find("proj_config");
		if (config == document.end() || !config->is_object())
			return;

		// Validate primitives before cereal reaches RapidJSON's typed accessors.
		// Invalid fields are absent for deserialization, so their declared defaults
		// survive without discarding valid neighboring settings.
		const auto sanitize = [&](const char* key, const auto& valid)
		{
			const auto field = config->find(key);
			if (field != config->end() && !valid(*field))
			{
				NEURUS_LOG("[ConfigComponent] invalid field '" << key << "' - kept default");
				config->erase(field);
			}
		};
		const auto floating = [](const auto& value)
		{
			if (!value.is_number()) return false;
			const double number = value.template get<double>();
			return std::isfinite(number) && std::abs(number) <= std::numeric_limits<float>::max()
				&& (number == 0 || std::abs(number) >= std::numeric_limits<float>::denorm_min());
		};
		const auto integer = [](const auto& value, long double minimum, long double maximum)
		{
			if (!value.is_number_integer()) return false;
			const long double number = value.template get<long double>();
			return number >= minimum && number <= maximum;
		};
		for (const char* key : {"r_gamma", "r_shadow_bias", "r_ao_radius", "r_fxaa_subpix",
		                        "r_fxaa_edge_threshold", "r_fxaa_edge_threshold_min"})
			sanitize(key, floating);
		for (const char* key : {"r_ao_ksize", "r_sample_pf", "r_sampling_count"})
			sanitize(key, [&](const auto& value) {
				return integer(value, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
			});
		sanitize("r_sampling_mode", [&](const auto& value) {
			return integer(value, 0, std::numeric_limits<uint32_t>::max());
		});
		for (const char* key : {"r_transparent", "r_debug_draw"})
			sanitize(key, [](const auto& value) { return value.is_boolean(); });
		const auto enumeration = [&](const char* key, int maximum) {
			sanitize(key, [&](const auto& value) { return integer(value, 0, maximum); });
		};
		enumeration("r_pipeline", static_cast<int>(RenderPipeLine::Custom0));
		enumeration("r_aa", static_cast<int>(AAAlg::FXAA));
		enumeration("r_ao", static_cast<int>(AOAlg::SSAO));
		enumeration("r_shadow", static_cast<int>(ShadowAlg::VSSM));
		enumeration("r_ssr", static_cast<int>(SSRAlg::SDFResolvedRayMarching));
		Serializable::LoadJson(document.dump());
	}
	catch (const nlohmann::json::exception& error)
	{
		NEURUS_LOG("[ConfigComponent] invalid config JSON: " << error.what() << " - using implicit defaults");
	}
}

} // namespace neurus::project
