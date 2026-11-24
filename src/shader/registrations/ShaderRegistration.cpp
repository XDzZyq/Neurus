/**
 * @file ShaderRegistration.cpp
 * @brief Cereal polymorphic registration for pooled shaders.
 *
 * Registers the concrete RenderShader and ComputeShader leaves of the
 * asset-layer ShaderData base, so the ResourceManager pool can serialize
 * pooled shaders by type name. ShaderData is abstract and is deliberately
 * not registered.
 */

#include <algorithm>
#include <cereal/archives/json.hpp>
#include <nlohmann/json.hpp>
#include "asset/ResourceType.hpp"

#include "core/UID.hpp"
#include "shader/ComputeShader.hpp"
#include "shader/RenderShader.hpp"

namespace
{
bool ValidShaderPayloadTypes(const nlohmann::ordered_json& value)
{
	if (value.is_array())
	{
		return std::all_of(value.begin(), value.end(), ValidShaderPayloadTypes);
	}
	if (!value.is_object())
		return true;
	for (const auto& [name, child] : value.items())
	{
		if ((name == "m_name" || name == "m_vertPath" || name == "m_fragPath"
			|| name == "m_compPath" || name == "path" || name == "code") && !child.is_string())
			return false;
		if (name == "parsed" && !child.is_object())
			return false;
		if (name == "stages")
		{
			if (!child.is_array()) return false;
			for (const auto& stage : child)
				if (!stage.is_object() || !stage.contains("key") || !stage["key"].is_number_integer()
					|| !stage.contains("value") || !stage["value"].is_object()) return false;
		}
		if (name == "m_defaults")
		{
			if (!child.is_array()) return false;
			for (const auto& entry : child)
				if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()
					|| !entry.contains("type") || !entry["type"].is_string()
					|| !entry.contains("value") || !entry["value"].is_string()) return false;
		}
		if (!ValidShaderPayloadTypes(child))
			return false;
	}
	return true;
}

bool UsableShader(const neurus::UID& resource)
{
	return static_cast<const neurus::ShaderData&>(resource).IsValid();
}

bool ValidShaderRecord(const nlohmann::ordered_json& data)
{
	return data.contains("stages") && ValidShaderPayloadTypes(data);
}
} // namespace

// --- Derived shader registration (concrete leaves only) ---
NEURUS_REGISTER_RESOURCE_TYPE(RenderShader, 1, ValidShaderRecord, UsableShader)
NEURUS_REGISTER_RESOURCE_TYPE(ComputeShader, 1, ValidShaderRecord, UsableShader)

// --- Polymorphic relations (UID base -> shader leaves) ---
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::RenderShader)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::ComputeShader)

CEREAL_REGISTER_DYNAMIC_INIT(neurus_shader_types)
