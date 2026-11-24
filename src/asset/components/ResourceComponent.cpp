/**
 * @file ResourceComponent.cpp
 * @brief ResourceManager pool persistence (see ResourceComponent.hpp).
 */

#include "asset/components/ResourceComponent.hpp"
#include "asset/components/ResourceArchiveWriter.hpp"
#include "asset/components/GraphRecordJson.hpp"

#include "core/ArchiveUid.hpp"
#include "core/Log.hpp"
#include "core/ResourceManager.hpp"
#include "core/ResourceType.hpp"

// Types needed for the pooled data-reference wiring in Load().
#include "asset/data/ImageData.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/render_graph/PassShaderCatalog.hpp"
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

std::string legacyName(const std::string& typeName);

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

	// Cereal puts each unnamed base at value0. Scene objects and concrete
	// shaders each have a named base between their own fields and UID.
	const char* nameField = ResourceNameField(typeName);
	const Json* base = &data;
	const Json* parent = nullptr;
	const int depth = *type == typeid(UID) ? 0 : (nameField && *type != typeid(ObjectID) ? 2 : 1);
	for (int i = 0; i < depth; ++i)
	{
		if (base->contains("o_id"))
			throw std::runtime_error("misplaced resource identity");
		parent = base;
		base = &base->at("value0");
	}
	if (base->contains("value0"))
		throw std::runtime_error("invalid resource identity base");
	std::string name;
	if (nameField && parent && parent->contains(nameField))
		name = parent->at(nameField).get<std::string>();
	else
		name = legacyName(typeName);
	const auto& key = entry.at("key");
	const auto& identity = base->at("o_id");
	if (!key.is_number_integer() || key < 0 || key >= std::numeric_limits<int>::max()
		|| !identity.is_number_integer() || identity != key)
		throw std::runtime_error("invalid resource identity");
	Json isolated;
	isolated["value"] = std::move(value);
	return {key.get<int>(), {*type, std::move(name), isolated.dump()}};
}

// These scene leaves use their unqualified type name as the constructor's
// display name when a legacy archive omits o_name. Sprite and ObjectID default
// to an empty string. Indexing legacy names must not construct pooled objects.
std::string legacyName(const std::string& typeName)
{
	for (const auto* namedType : {"neurus::Camera", "neurus::Light", "neurus::Mesh",
	     "neurus::Environment", "neurus::DebugLine", "neurus::DebugPoints", "neurus::DebugMesh"})
		if (typeName == namedType)
			return typeName.substr(sizeof("neurus::") - 1);
	return {};
}

void restoreDependencies(ResourceManager& pool, const std::shared_ptr<UID>& resource)
{
	if (auto mesh = std::dynamic_pointer_cast<Mesh>(resource))
	{
		if (mesh->o_meshDataId != 0)
		{
			mesh->o_mesh = pool.Get<MeshData>(mesh->o_meshDataId);
			if (!mesh->o_mesh)
				NEURUS_ERR("[ResourceComponent] stale MeshData UID " << mesh->o_meshDataId);
		}
		auto shader = mesh->o_shaderId == 0 ? nullptr : pool.Get<ShaderData>(mesh->o_shaderId);
		if (shader && shader->GetType() != ShaderType::VERTEX)
			shader.reset();
		mesh->SetObjShader(shader);
	}
	else if (auto debug = std::dynamic_pointer_cast<DebugMesh>(resource); debug && debug->o_meshDataId != 0)
	{
		debug->o_mesh = pool.Get<MeshData>(debug->o_meshDataId);
		if (!debug->o_mesh)
			NEURUS_ERR("[ResourceComponent] stale MeshData UID " << debug->o_meshDataId << " on DebugMesh");
	}
	else if (auto env = std::dynamic_pointer_cast<Environment>(resource); env && env->o_imageDataId != 0)
	{
		if (auto image = pool.Get<ImageData>(env->o_imageDataId))
			env->SetEquirectData(std::move(image));
		else
			NEURUS_ERR("[ResourceComponent] stale ImageData UID " << env->o_imageDataId);
	}
}

using DiagnosticList = std::reference_wrapper<std::vector<std::string>>;

void reportGraphDiagnostics(int id, const std::vector<std::string>& messages,
                            const std::weak_ptr<DiagnosticList>& sink)
{
	for (const auto& message : messages)
	{
		const auto text = "Graph UID " + std::to_string(id) + ": " + message;
		NEURUS_LOG("[ResourceComponent] " << text);
		if (const auto list = sink.lock())
			list->get().push_back(text);
	}
}

void applyRegisteredName(const std::shared_ptr<UID>& resource, const std::string& name)
{
	if (auto object = std::dynamic_pointer_cast<ObjectID>(resource))
		object->o_name = name;
	else if (auto shader = std::dynamic_pointer_cast<ShaderData>(resource))
		shader->SetName(name);
}

void registerRecord(ResourceManager& pool, int id, ResourceRecord& record,
                    const std::shared_ptr<const IPassShaderCatalog>& catalog,
                    std::weak_ptr<DiagnosticList> diagnostics)
{
	auto payload = std::make_shared<const std::string>(std::move(record.payload));
	const bool isGraph = record.type == typeid(RenderGraphData);
	auto load = [&pool, id, payload, isGraph, catalog, diagnostics]() -> std::shared_ptr<UID>
	{
		try
		{
			if (isGraph)
			{
				const auto document = Json::parse(*payload);
				std::istringstream stream(document.at("value").at("ptr_wrapper").at("data").dump());
				cereal::JSONInputArchive archive(stream);
				RenderGraphRecord topology;
				archive(cereal::make_nvp("topology", topology));
				std::vector<std::string> messages;
				auto graph = RestoreGraph(topology, *catalog, pool, messages, id);
				reportGraphDiagnostics(id, messages, diagnostics);
				return graph;
			}
			std::shared_ptr<UID> resource;
			{
				std::istringstream stream(*payload);
				cereal::JSONInputArchive archive(stream);
				const ArchiveUid identity(id);
				archive(cereal::make_nvp("value", resource));
			}
			if (!resource || resource->GetObjectID() != id)
				throw std::runtime_error("restored resource identity differs from its indexed UID");
			if (const auto shader = std::dynamic_pointer_cast<ShaderData>(resource); shader && !shader->IsValid())
			{
				NEURUS_LOG("[ResourceComponent] Unusable shader UID " << id);
				return nullptr;
			}
			restoreDependencies(pool, resource);
			applyRegisteredName(resource, pool.RegisteredName(id));
			return resource;
		}
		catch (const std::exception& error)
		{
			NEURUS_LOG("[ResourceComponent] Could not restore UID " << id << ": " << error.what());
			return nullptr;
		}
	};
	pool.RegisterDeferred(id, record.type, std::move(load), payload);
}

} // namespace

ResourceComponent::ResourceComponent(ResourceManager& resources, const IPassShaderCatalog& catalog,
                                     std::vector<std::string>* diagnostics)
	: m_resources(&resources), m_catalog(catalog), m_diagnostics(diagnostics ? std::make_shared<DiagnosticList>(*diagnostics) : nullptr)
{
}

void ResourceComponent::Save(cereal::JSONOutputArchive& ar) const
{
	WriteResourcePool(ar, *m_resources);
}

void ResourceComponent::Load(cereal::JSONInputArchive& ar)
{
	GraphArchiveRecords graphs(ar);
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

	// Direct cereal callers already supplied an archive of live objects. Complete
	// those objects in dependency order using the same wiring as indexed reads.
	std::vector<int> ids;
	m_resources->ForEachUID([&](const auto& resource) { ids.push_back(resource->GetObjectID()); });
	std::sort(ids.begin(), ids.end());
	for (const int id : ids)
	{
		const auto shader = m_resources->Get<ShaderData>(id);
		if (shader && !shader->IsValid())
			m_resources->Remove(id);
	}
	for (const auto& [id, topology] : graphs.topologies)
	{
		const auto graph = m_resources->Get<RenderGraphData>(id);
		if (!graph)
			continue;
		std::vector<std::string> messages;
		auto restored = RestoreGraph(topology, m_catalog, *m_resources, messages, id);
		reportGraphDiagnostics(id, messages, m_diagnostics);
		if (restored)
			graph->SwapContents(*restored);
		else
			m_resources->Remove(id);
	}
	for (const int id : ids)
	{
		const auto resource = m_resources->Get(id);
		restoreDependencies(*m_resources, resource);
		std::string name;
		if (auto object = std::dynamic_pointer_cast<ObjectID>(resource))
			name = object->o_name;
		else if (auto shader = std::dynamic_pointer_cast<ShaderData>(resource))
			name = shader->GetName();
		if (!name.empty())
			applyRegisteredName(resource, m_resources->SetName(id, name));
	}
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

	// Every loader is registered before a duplicate check or root lookup can
	// recursively request a dependency. Names are claimed in ascending UID order.
	const auto catalog = m_catalog.Share();
	for (auto& [id, candidates] : records)
		registerRecord(*m_resources, id, candidates.front(), catalog, m_diagnostics);
	// Only ambiguous records require a probe to select the first usable entry.
	for (auto& [id, candidates] : records)
	{
		const ResourceRecord* accepted = &candidates.front();
		if (candidates.size() > 1)
		{
			accepted = nullptr;
			for (auto& candidate : candidates)
			{
				if (&candidate != &candidates.front())
					registerRecord(*m_resources, id, candidate, catalog, m_diagnostics);
				if (m_resources->Get(id))
				{
					accepted = &candidate;
					break;
				}
			}
		}
		if (accepted && !accepted->name.empty())
		{
			const auto name = m_resources->SetName(id, accepted->name);
			if (const auto resource = m_resources->GetLive(id))
				applyRegisteredName(resource, name);
		}
	}
}

} // namespace neurus::project
