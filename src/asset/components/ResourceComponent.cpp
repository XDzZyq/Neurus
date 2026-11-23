/**
 * @file ResourceComponent.cpp
 * @brief ResourceManager pool persistence (see ResourceComponent.hpp).
 */

#include "asset/components/ResourceComponent.hpp"
#include "asset/components/GraphRecordJson.hpp"

#include "core/Log.hpp"
#include "core/ResourceManager.hpp"

// Types needed for the pooled data-reference wiring in Load().
#include "asset/data/ImageData.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/MeshData.hpp"
#include "scene/Environment.hpp"
#include "scene/DebugMesh.hpp"
#include "scene/Mesh.hpp"
#include "scene/ObjectID.hpp"
#include "asset/data/ShaderData.hpp"

// Force-link the polymorphic registration TUs (static libraries) so the pool's
// UID-derived types are registered before serialization runs.
#include "scene/registrations/TypeRegistration.hpp"
#include "asset/registrations/DataRegistration.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <vector>

namespace neurus::project
{

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
		if (name == "o_id" && !child.is_number_integer())
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

} // namespace

ResourceComponent::ResourceComponent(ResourceManager& resources, const IPassShaderCatalog& catalog,
                                     std::vector<std::string>* diagnostics)
	: m_resources(&resources), m_catalog(catalog), m_diagnostics(diagnostics)
{
}

void ResourceComponent::Save(cereal::JSONOutputArchive& ar) const
{
	ar(cereal::make_nvp("m_resources", *m_resources));
}

void ResourceComponent::Load(cereal::JSONInputArchive& ar)
{
	try
	{
		ar(cereal::make_nvp("m_resources", *m_resources));
	}
	catch (const cereal::Exception& e)
	{
		// Legacy project file (no "m_resources" node): leave the pool empty.
		NEURUS_ERR("ResourceComponent::Load: " << e.what() << " - using empty resource pool.");
		m_resources->Clear();
	}

	RestoreReferences();
}

void ResourceComponent::LoadJson(const std::string& object)
{
	m_resources->Clear();
	const auto document = nlohmann::ordered_json::parse(object);
	const auto pool = document.find("m_resources");
	if (pool == document.end() || !pool->is_object()
		|| !pool->contains("resources_") || !(*pool)["resources_"].is_array())
	{
		NEURUS_LOG("[ResourceComponent] No usable resource pool; keeping project resources absent");
		return;
	}

	// Each resource gets its own archive, so a malformed shader cannot strand
	// the cursor inside its object and prevent later meshes/data from loading.
	// Cereal names a polymorphic type only on its first occurrence; carry that
	// dictionary forward while making every isolated entry self-contained.
	std::unordered_map<uint32_t, std::string> typeNames;
	std::unordered_set<int> loadedIds;
	std::vector<std::shared_ptr<UID>> loaded;
	for (auto entry : (*pool)["resources_"])
	{
		try
		{
			auto& value = entry.at("value");
			const uint32_t type = value.at("polymorphic_id").get<uint32_t>();
			if (type == 0)
				continue;
			constexpr uint32_t definition = 0x80000000u;
			const uint32_t typeId = type & ~definition;
			if ((type & definition) != 0)
				typeNames[typeId] = value.at("polymorphic_name").get<std::string>();
			else if ((type & 0x40000000u) == 0)
			{
				value["polymorphic_id"] = typeId | definition;
				value["polymorphic_name"] = typeNames.at(typeId);
			}

			const int id = entry.at("key").get<int>();
			const auto& wrapper = value.at("ptr_wrapper");
			if (!wrapper.is_object() || !wrapper.contains("id") || !wrapper["id"].is_number_unsigned()
				|| !wrapper.contains("data") || !wrapper["data"].is_object())
				throw std::runtime_error("invalid resource pointer payload");
			const std::string typeName = value.value("polymorphic_name", "");
			if (typeName.find("Shader") != std::string::npos && !ValidShaderPayloadTypes(wrapper["data"]))
				throw std::runtime_error("invalid shader payload field type");

			if (typeName == "neurus::RenderGraphData")
			{
				const auto& data = wrapper.at("data");
				const auto& identity = data.at("value0").at("o_id");
				if (!entry.at("key").is_number_integer() || id < 0 || id == std::numeric_limits<int>::max()
					|| !identity.is_number_integer() || identity != id || entry.at("key") != id
					|| !ValidGraphShape(data.at("topology")))
					throw std::runtime_error("invalid render graph identity or topology");
			}

			nlohmann::ordered_json single;
			single["m_resources"]["resources_"] = nlohmann::ordered_json::array({entry});
			std::stringstream stream(single.dump());
			ResourceManager staging;
			cereal::JSONInputArchive archive(stream);
			archive(cereal::make_nvp("m_resources", staging));
			const auto resource = staging.Get<UID>(id);
			if (!resource || resource->GetObjectID() != id || !loadedIds.insert(id).second)
				throw std::runtime_error("invalid or duplicate resource identity");
			loaded.push_back(resource);
		}
		catch (const std::exception& error)
		{
			NEURUS_LOG("[ResourceComponent] Skipped unusable resource: " << error.what());
		}
	}

	// Registration seeds object names; UID order keeps duplicate-name suffixes
	// deterministic regardless of the archived unordered-map order.
	std::sort(loaded.begin(), loaded.end(), [](const auto& left, const auto& right)
	{
		return left->GetObjectID() < right->GetObjectID();
	});
	for (auto& resource : loaded)
		m_resources->Register(std::move(resource));
	RestoreReferences();
}

void ResourceComponent::RestoreReferences()
{
	// A shader that could not restore its source or compile is absent project
	// data. Drop it before wiring consumers; the renderer supplies cached built-ins
	// without inserting a replacement into this pool.
	std::vector<int> invalidShaders;
	m_resources->ForEachUID([&invalidShaders](const std::shared_ptr<UID>& resource)
	{
		const auto shader = std::dynamic_pointer_cast<ShaderData>(resource);
		if (shader && !shader->IsValid())
			invalidShaders.push_back(shader->GetObjectID());
	});
	for (const int uid : invalidShaders)
	{
		NEURUS_LOG("[ResourceComponent] Shader " << uid
		           << " could not be restored; using the built-in fallback");
		m_resources->Remove(uid);
	}

	// Resolve inactive graphs too. A graph may appear before its shaders in the archive.
	std::vector<int> invalidGraphs;
	m_resources->ForEach<RenderGraphData>([&](const std::shared_ptr<RenderGraphData>& graph)
	{
		std::vector<std::string> diagnostics;
		if (!graph->Resolve(m_catalog, *m_resources, diagnostics))
			invalidGraphs.push_back(graph->GetObjectID());
		for (const auto& diagnostic : diagnostics)
		{
			const auto message = "Graph UID " + std::to_string(graph->GetObjectID()) + ": " + diagnostic;
			NEURUS_LOG("[ResourceComponent] " << message);
			if (m_diagnostics)
				m_diagnostics->push_back(message);
		}
	});
	for (const int uid : invalidGraphs)
		m_resources->Remove(uid);

	// Wire data references for EVERY pooled mesh/environment, not just the
	// scene members: a pooled object can stay referenced elsewhere after
	// deletion, so its data refs must be wired for on-demand GPU uploads
	// after a reload. Pure pointer wiring - the pool's data-resource entries
	// have already reloaded their content via their own serialize(load).
	m_resources->ForEach<Mesh>([&](const std::shared_ptr<Mesh>& mesh) {
		if (!mesh) return;
		if (mesh->o_meshDataId != 0)
		{
			auto meshData = m_resources->Get<MeshData>(mesh->o_meshDataId);
			if (meshData)
				mesh->o_mesh = meshData;
			else
				NEURUS_ERR("[ResourceComponent] stale MeshData UID " << mesh->o_meshDataId);
		}
		// SetObjShader also clears a missing/failed reference's serialized UID.
		auto shader = mesh->o_shaderId == 0
			? nullptr : m_resources->Get<ShaderData>(mesh->o_shaderId);
		if (shader && shader->GetType() != ShaderType::VERTEX)
			shader.reset();
		if (mesh->o_shaderId != 0 && !shader)
			NEURUS_LOG("[ResourceComponent] Shader " << mesh->o_shaderId
			           << " is absent; mesh uses the built-in fallback");
		mesh->SetObjShader(shader);
	});
	// DebugMesh carries the same MeshData reference as Mesh but is not a Mesh,
	// so ForEach<Mesh> above never reaches it.
	m_resources->ForEach<DebugMesh>([&](const std::shared_ptr<DebugMesh>& dMesh) {
		if (!dMesh || dMesh->o_meshDataId == 0) return;
		auto meshData = m_resources->Get<MeshData>(dMesh->o_meshDataId);
		if (meshData)
			dMesh->o_mesh = meshData;
		else
			NEURUS_ERR("[ResourceComponent] stale MeshData UID " << dMesh->o_meshDataId
			           << " on DebugMesh");
	});
	m_resources->ForEach<Environment>([&](const std::shared_ptr<Environment>& env) {
		if (!env || env->o_imageDataId == 0) return;
		auto imageData = m_resources->Get<ImageData>(env->o_imageDataId);
		if (!imageData)
		{
			// Keep o_imageDataId (do not zero it) so the broken reference
			// stays visible and reported instead of being silently dropped.
			NEURUS_ERR("[ResourceComponent] stale ImageData UID " << env->o_imageDataId);
			return;
		}
		env->SetEquirectData(imageData);
	});

	// Seed the name registry from the restored object names. Sorting by UID keeps
	// duplicate-name suffixing deterministic; writing the returned unique name back
	// keeps o_name and the registry in sync across saves.
	//
	// ForEachUID, not ForEach<ObjectID>: the latter keys the EXACT dynamic type,
	// so a pooled base ObjectID would hide every camera/mesh/light from the seed.
	std::vector<std::pair<int, std::string>> named;
	m_resources->ForEachUID([&](const std::shared_ptr<UID>& resource)
	{
		auto obj = std::dynamic_pointer_cast<ObjectID>(resource);
		if (obj && !obj->o_name.empty())
			named.emplace_back(obj->GetObjectID(), obj->o_name);
	});
	std::sort(named.begin(), named.end(),
	          [](const auto& l, const auto& r) { return l.first < r.first; });
	for (const auto& [id, name] : named)
	{
		const std::string unique = m_resources->SetName(id, name);
		if (auto obj = m_resources->Get<ObjectID>(id))
			obj->o_name = unique.empty() ? name : unique;
	}

	// Pooled shaders are data resources addressed by name
	// (neurus.data.shaders["Name"]), so re-seed their registry entries from the
	// serialized ShaderData name. Sorting by UID keeps duplicate suffixing
	// deterministic, like the object seed above.
	std::vector<std::pair<int, std::string>> shaderNames;
	m_resources->ForEachUID([&](const std::shared_ptr<UID>& resource)
	{
		auto shader = std::dynamic_pointer_cast<ShaderData>(resource);
		if (shader && !shader->GetName().empty())
			shaderNames.emplace_back(shader->GetObjectID(), shader->GetName());
	});
	std::sort(shaderNames.begin(), shaderNames.end(),
	          [](const auto& l, const auto& r) { return l.first < r.first; });
	for (const auto& [id, name] : shaderNames)
	{
		const std::string unique = m_resources->SetName(id, name);
		if (auto shader = m_resources->Get<ShaderData>(id))
			shader->SetName(unique.empty() ? name : unique);
	}
}

} // namespace neurus::project
