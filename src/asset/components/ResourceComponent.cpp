/**
 * @file ResourceComponent.cpp
 * @brief ResourceManager pool persistence (see ResourceComponent.hpp).
 */

#include "asset/components/ResourceComponent.hpp"
#include "asset/components/GraphRecordJson.hpp"

#include "core/ArchiveUid.hpp"
#include "core/Log.hpp"
#include "core/ResourceManager.hpp"
#include "core/ResourceType.hpp"

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
#include <map>
#include <unordered_map>
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

using Json = nlohmann::ordered_json;

struct ResourceRecord
{
	std::type_index type;
	std::string name;
	std::string payload;
};

// Cereal writes the concrete type name once per archive. Make each retained
// pointer self-contained before the archive and its type dictionary go away.
std::string normalizeType(Json& value, std::unordered_map<uint32_t, std::string>& typeNames)
{
	constexpr uint32_t definition = 0x80000000u;
	constexpr uint32_t staticType = 0x40000000u;
	const auto& encoded = value.at("polymorphic_id");
	if (!encoded.is_number_unsigned() || encoded.get<uint64_t>() > std::numeric_limits<uint32_t>::max())
		throw std::runtime_error("invalid resource type ID");
	const uint32_t type = encoded.get<uint32_t>();
	if (type == 0)
		return {}; // Cereal's null pointer marker.
	if (type == staticType)
		return "neurus::UID";

	const uint32_t typeId = type & ~definition;
	if (typeId == 0 || (typeId & staticType) != 0)
		throw std::runtime_error("invalid resource type ID");
	if ((type & definition) != 0)
		typeNames[typeId] = value.at("polymorphic_name").get<std::string>();
	else
	{
		value["polymorphic_id"] = typeId | definition;
		value["polymorphic_name"] = typeNames.at(typeId);
	}
	return typeNames.at(typeId);
}

std::pair<int, ResourceRecord> readRecord(Json& entry, const std::string& typeName)
{
	const auto type = FindResourceType(typeName);
	if (!type)
		throw std::runtime_error("unknown resource type: " + typeName);

	auto& value = entry.at("value");
	const auto& wrapper = value.at("ptr_wrapper");
	const auto& pointer = wrapper.at("id");
	if (!pointer.is_number_unsigned() || pointer.get<uint64_t>() > std::numeric_limits<uint32_t>::max()
		|| (pointer.get<uint32_t>() & 0x80000000u) == 0)
		throw std::runtime_error("invalid resource pointer payload");
	const auto& data = wrapper.at("data");
	const bool shader = typeName == "neurus::RenderShader" || typeName == "neurus::ComputeShader";
	if (shader && !ValidShaderPayloadTypes(data))
		throw std::runtime_error("invalid shader payload field type");
	if (*type == typeid(RenderGraphData) && !ValidGraphShape(data.at("topology")))
		throw std::runtime_error("invalid render graph topology");

	// Cereal's unnamed base-class field is value0. Only follow that chain
	// for identity; an unrelated field named o_id cannot identify a resource.
	const Json* base = &data;
	const Json* parent = nullptr;
	while (base->contains("value0"))
	{
		parent = base;
		base = &base->at("value0");
	}
	const char* nameField = shader ? "m_name" : "o_name";
	std::string name;
	if (parent && parent->contains(nameField))
		name = parent->at(nameField).get<std::string>();
	const auto& key = entry.at("key");
	const auto& identity = base->at("o_id");
	if (!key.is_number_integer() || key < 0 || key >= std::numeric_limits<int>::max()
		|| !identity.is_number_integer() || identity != key)
		throw std::runtime_error("invalid resource identity");
	Json isolated;
	isolated["value"] = std::move(value);
	return {key.get<int>(), {*type, std::move(name), isolated.dump()}};
}

void registerRecord(ResourceManager& pool, int id, ResourceRecord& record)
{
	pool.RegisterDeferred(id, record.type, [id, payload = std::move(record.payload)]
	{
		std::istringstream stream(payload);
		cereal::JSONInputArchive archive(stream);
		std::shared_ptr<UID> resource;
		const ArchiveUid identity(id);
		archive(cereal::make_nvp("value", resource));
		return resource;
	});
}

// The legacy archive reader has only live objects. A project record may also
// omit an optional name, in which case its constructor supplies the default.
void seedName(ResourceManager& pool, const std::shared_ptr<UID>& resource, const std::string& recorded = {})
{
	if (auto object = std::dynamic_pointer_cast<ObjectID>(resource))
	{
		const auto& name = recorded.empty() ? object->o_name : recorded;
		if (!name.empty())
			object->o_name = pool.SetName(object->GetObjectID(), name);
	}
	else if (auto shader = std::dynamic_pointer_cast<ShaderData>(resource))
	{
		const auto& name = recorded.empty() ? shader->GetName() : recorded;
		if (!name.empty())
			shader->SetName(pool.SetName(shader->GetObjectID(), name));
	}
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
	std::vector<int> ids;
	m_resources->ForEachUID([&](const std::shared_ptr<UID>& resource) { ids.push_back(resource->GetObjectID()); });
	std::sort(ids.begin(), ids.end());
	for (const int id : ids)
		seedName(*m_resources, m_resources->Get(id));
}

void ResourceComponent::LoadJson(const std::string& object)
{
	m_resources->Clear();
	auto document = Json::parse(object);
	const auto pool = document.find("m_resources");
	if (pool == document.end() || !pool->is_object()
		|| !pool->contains("resources_") || !(*pool)["resources_"].is_array())
	{
		NEURUS_LOG("[ResourceComponent] No usable resource pool; keeping project resources absent");
		return;
	}

	// A broken defining entry can still name the type of a valid sibling.
	std::unordered_map<uint32_t, std::string> typeNames;
	std::map<int, std::vector<ResourceRecord>> records;
	for (auto& entry : pool->at("resources_"))
	{
		try
		{
			const auto typeName = normalizeType(entry.at("value"), typeNames);
			if (!typeName.empty())
			{
				auto [id, record] = readRecord(entry, typeName);
				records[id].push_back(std::move(record));
			}
		}
		catch (const std::exception& error)
		{
			NEURUS_LOG("[ResourceComponent] Skipped unusable resource: " << error.what());
		}
	}

	// Reserve every identity and seed the type index in UID order
	// before a constructor can allocate another ID or a lookup can run.
	for (auto& [id, candidates] : records)
		registerRecord(*m_resources, id, candidates.front());

	// The current whole-pool fixup needs all records materialized. Isolate
	// failures here and retain the first readable entry for a duplicate UID.
	for (auto& [id, candidates] : records)
	{
		for (auto& candidate : candidates)
		{
			try
			{
				if (!m_resources->Contains(id))
					registerRecord(*m_resources, id, candidate);
				m_resources->Get(id);
				// Keep the accepted candidate's name for the ordered name index.
				if (&candidate != &candidates.front())
					candidates.front().name = std::move(candidate.name);
				break;
			}
			catch (const std::exception& error)
			{
				m_resources->Remove(id);
				NEURUS_LOG("[ResourceComponent] Skipped unusable resource " << id << ": " << error.what());
			}
		}
	}
	RestoreReferences();
	// Claim names only after rejecting bad records. A legacy constructor default
	// participates in UID order alongside explicitly named records.
	for (const auto& [id, candidates] : records)
		seedName(*m_resources, m_resources->Get(id), candidates.front().name);
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
}

} // namespace neurus::project
