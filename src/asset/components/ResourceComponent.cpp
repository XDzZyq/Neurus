/**
 * @file ResourceComponent.cpp
 * @brief ResourceManager pool persistence (see ResourceComponent.hpp).
 */

#include "asset/components/ResourceComponent.hpp"

#include "core/Log.hpp"
#include "asset/ResourceManager.hpp"
#include "asset/ResourceType.hpp"

// Force-link the polymorphic registration TUs (static libraries) so the pool's
// UID-derived types are registered before serialization runs.
#include "scene/registrations/TypeRegistration.hpp"
#include "asset/registrations/DataRegistration.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <map>
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
	const ResourceTypeRegistration* type;
	std::string name;
	std::string payload;
};

std::pair<int, ResourceRecord> readRecord(const Json& entry)
{
	const auto& key = entry.at("key");
	if (!key.is_number_integer() || key < 0 || key >= std::numeric_limits<int>::max())
		throw std::runtime_error("invalid resource UID");
	const auto& typeName = entry.at("type").get_ref<const std::string&>();
	const auto* type = ResourceTypeRegistration::Find(typeName);
	if (!type)
		throw std::runtime_error("unknown resource type: " + typeName);
	const auto& data = entry.at("data");
	if (!data.is_object() || (type->validPayload && !type->validPayload(data)))
		throw std::runtime_error("invalid pooled resource payload");

	std::string name;
	if (type->nameField)
	{
		const auto& fields = type->nameDepth == 0 ? data : data.at("value0");
		if (!fields.is_object())
			throw std::runtime_error("invalid pooled name fields");
		if (fields.contains(type->nameField))
			name = fields.at(type->nameField).get<std::string>();
	}
	return {key.get<int>(), {type, std::move(name), entry.dump()}};
}

void registerRecord(ResourceManager& pool, int id, ResourceRecord& record,
                    const std::shared_ptr<const ResourceReadServices>& services)
{
	auto payload = std::make_shared<const std::string>(std::move(record.payload));
	auto load = [&pool, id, payload, services, type = record.type]() -> std::shared_ptr<UID>
	{
		try
		{
			std::shared_ptr<UID> resource;
			{
				std::istringstream stream(*payload);
				cereal::JSONInputArchive archive(stream);
				ArchiveResourceRefs refs(archive, services.get());
				resource = type->readBody(archive, RestoredId{id});
				if (!refs.Resolve(pool).empty()) return nullptr;
			}
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
	pool.RegisterDeferred(id, record.type->type, std::move(load), payload);
}

} // namespace

ResourceComponent::ResourceComponent(ResourceManager& resources, std::shared_ptr<const ResourceReadServices> services)
	: m_resources(resources), m_services(std::move(services))
{
}

void ResourceComponent::Save(cereal::JSONOutputArchive& ar) const
{
	ar(cereal::make_nvp("m_resources", m_resources));
}

void ResourceComponent::Load(cereal::JSONInputArchive& ar)
{
	ArchiveResourceRefs scope(ar, m_services.get());
	try
	{
		ar(cereal::make_nvp("m_resources", m_resources));
	}
	catch (const std::exception& e)
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

	std::map<int, std::vector<ResourceRecord>> records;
	for (auto& entry : pool->at("resources_"))
	{
		try
		{
			auto [id, record] = readRecord(entry);
			records[id].push_back(std::move(record));
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
