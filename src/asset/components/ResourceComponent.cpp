/**
 * @file ResourceComponent.cpp
 * @brief ResourceManager pool persistence (see ResourceComponent.hpp).
 */

#include "asset/components/ResourceComponent.hpp"
#include "asset/components/ResourceArchiveWriter.hpp"

#include "core/ArchiveUID.hpp"
#include "core/Log.hpp"
#include "asset/ResourceManager.hpp"
#include "asset/ResourceType.hpp"

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
	if (!ValidResourcePayload(typeName, data))
		throw std::runtime_error("invalid pooled resource payload");

	// Cereal puts each unnamed base at value0. Scene objects and concrete
	// shaders each have a named base between their own fields and UID.
	const char* nameField = ResourceNameField(typeName);
	const Json* base = &data;
	const Json* parent = nullptr;
	const int depth = ResourceIdentityDepth(typeName);
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
	const auto& key = entry.at("key");
	const auto& identity = base->at("o_id");
	if (!key.is_number_integer() || key < 0 || key >= std::numeric_limits<int>::max()
		|| !identity.is_number_integer() || identity != key)
		throw std::runtime_error("invalid resource identity");
	Json isolated;
	isolated["value"] = std::move(value);
	return {key.get<int>(), {*type, std::move(name), isolated.dump()}};
}

void registerRecord(ResourceManager& pool, int id, ResourceRecord& record,
                    const std::shared_ptr<const ResourceReadServices>& services)
{
	auto payload = std::make_shared<const std::string>(std::move(record.payload));
	auto load = [&pool, id, payload, services]() -> std::shared_ptr<UID>
	{
		try
		{
			std::shared_ptr<UID> resource;
			{
				std::istringstream stream(*payload);
				cereal::JSONInputArchive archive(stream);
				ArchiveResourceRefs refs(archive, services.get());
				const ArchiveUID identity(id);
				archive(cereal::make_nvp("value", resource));
				if (!refs.Resolve(pool).empty()) return nullptr;
			}
			if (!resource || resource->GetObjectID() != id)
				throw std::runtime_error("restored resource identity differs from its indexed UID");
			if (!IsUsableResource(*resource))
				return nullptr;
			WriteResourceName(*resource, pool.RegisteredName(id));
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

ResourceComponent::ResourceComponent(ResourceManager& resources, std::shared_ptr<const ResourceReadServices> services)
	: m_resources(resources), m_services(std::move(services))
{
}

void ResourceComponent::Save(cereal::JSONOutputArchive& ar) const
{
	WriteResourcePool(ar, m_resources);
}

void ResourceComponent::Load(cereal::JSONInputArchive& ar)
{
	ArchiveResourceRefs scope(ar, m_services.get());
	try
	{
		ar(cereal::make_nvp("m_resources", m_resources));
	}
	catch (const cereal::Exception& e)
	{
		// A broken pool must not leave objects from the previous project active.
		NEURUS_ERR("ResourceComponent::Load: " << e.what() << " - using empty resource pool.");
		m_resources.Clear();
	}

	// The direct cereal reader already constructed these objects. Claim names
	// in the same UID order as the deferred project reader.
	auto ids = m_resources.RegisteredIds();
	std::sort(ids.begin(), ids.end());
	for (const int id : ids)
	{
		const auto resource = m_resources.GetLive(id);
		if (!resource) continue;
		if (!IsUsableResource(*resource))
		{
			m_resources.Remove(id);
			continue;
		}
		const auto name = ReadResourceName(*resource);
		if (!name.empty())
			WriteResourceName(*resource, m_resources.SetName(id, name));
	}
}

void ResourceComponent::LoadJson(const std::string& object)
{
	m_resources.Clear();
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
	for (auto& [id, candidates] : records)
		registerRecord(m_resources, id, candidates.front(), m_services);
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
					registerRecord(m_resources, id, candidate, m_services);
				if (m_resources.Get(id))
				{
					accepted = &candidate;
					break;
				}
			}
		}
		if (accepted && !accepted->name.empty())
		{
			const auto name = m_resources.SetName(id, accepted->name);
			if (const auto resource = m_resources.GetLive(id))
				WriteResourceName(*resource, name);
		}
	}
}

} // namespace neurus::project
