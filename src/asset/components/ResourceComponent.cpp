/**
 * @file ResourceComponent.cpp
 * @brief ResourceManager pool persistence (see ResourceComponent.hpp).
 */

#include "asset/components/ResourceComponent.hpp"

#include "core/Log.hpp"
#include "asset/ResourceManager.hpp"
#include "asset/ResourceInputArchive.hpp"
#include "asset/ResourceType.hpp"

// Force-link the polymorphic registration TUs (static libraries) so the pool's
// UID-derived types are registered before serialization runs.
#include "scene/registrations/TypeRegistration.hpp"
#include "asset/registrations/DataRegistration.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <sstream>
#include <stdexcept>
#include <map>
#include <memory>
#include <string>
#include <utility>

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
	if (!entry.is_object())
		throw std::runtime_error("resource record must be an object");
	if (entry.contains("value") && !entry.contains("data"))
		throw std::runtime_error("legacy {key, value} resource record is not supported");
	const auto& key = entry.at("key");
	if (!key.is_number_integer() || key < 0 || key >= std::numeric_limits<int>::max())
		throw std::runtime_error("invalid resource UID");
	const auto& type_name = entry.at("type").get_ref<const std::string&>();
	const auto* type = ResourceTypeRegistration::Find(type_name);
	if (!type)
		throw std::runtime_error("unknown resource type: " + type_name);
	const auto& data = entry.at("data");
	if (!data.is_object() || (type->ValidPayload && !type->ValidPayload(data)))
		throw std::runtime_error("invalid pooled resource payload");

	std::string name;
	if (!type->name_field.empty())
	{
		const auto& fields = type->name_depth == 0 ? data : data.at("value0");
		if (!fields.is_object())
			throw std::runtime_error("invalid pooled name fields");
		name = fields.contains(type->name_field)
			? fields.at(type->name_field).get<std::string>()
			: type->DefaultName();
	}
	return {key.get<int>(), {type, std::move(name), entry.dump()}};
}

void registerRecord(ResourceManager& pool, int id, ResourceRecord& record)
{
	auto payload = std::make_shared<const std::string>(std::move(record.payload));
	auto populate = [&pool, id, payload, type = record.type](UID& resource)
	{
		try
		{
			std::istringstream stream(*payload);
			ResourceInputArchive archive(stream, pool);
			type->ReadBody(archive, resource);
			if (!IsUsableResource(resource))
			{
				NEURUS_ERR("[ResourceComponent] UID " << id << " is not usable; dropping it from the project");
				return false;
			}
			WriteResourceName(resource, pool.RegisteredName(id));
			return true;
		}
		catch (const std::exception& error)
		{
			NEURUS_ERR("[ResourceComponent] Could not restore UID " << id << ": " << error.what());
			return false;
		}
	};
	pool.RegisterDeferred(id, record.type->type, record.type->Create, payload, std::move(populate));
}

} // namespace

ResourceComponent::ResourceComponent(ResourceManager& resources)
	: m_resources(resources)
{
}

void ResourceComponent::Save(cereal::JSONOutputArchive& ar) const
{
	ar(cereal::make_nvp("m_resources", m_resources));
}

void ResourceComponent::LoadJson(const std::string& object)
{
	// Parse before touching the pool: a corrupt file must fail the open rather
	// than leave the document empty.
	auto document = Json::parse(object);
	const auto pool = document.find("m_resources");
	if (pool == document.end())
		throw std::runtime_error("Project has no resource pool (m_resources)");
	if (!pool->is_object())
		throw std::runtime_error("Resource pool (m_resources) must be an object");
	if (!pool->contains("resources_") || !(*pool)["resources_"].is_array())
		throw std::runtime_error("Resource pool has no resources_ array");

	std::map<int, ResourceRecord> records;
	int index = 0;
	for (const auto& entry : pool->at("resources_"))
	{
		try
		{
			auto [id, record] = readRecord(entry);
			if (!records.try_emplace(id, std::move(record)).second)
				throw std::runtime_error("duplicate resource UID " + std::to_string(id));
		}
		catch (const std::exception& error)
		{
			throw std::runtime_error(
				"Unreadable resource record #" + std::to_string(index) + ": " + error.what());
		}
		++index;
	}

	// Every record validated; only now replace the pool.
	m_resources.Clear();

	// Claim names in ascending UID order without loading any of the objects.
	for (auto& [id, record] : records)
	{
		registerRecord(m_resources, id, record);
		if (!record.type->name_field.empty())
			m_resources.SetName(id, record.name);
	}
}

} // namespace neurus::project
