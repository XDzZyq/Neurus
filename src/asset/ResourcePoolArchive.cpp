#include "asset/ResourceManager.hpp"
#include "asset/ResourceType.hpp"

#include <cereal/archives/json.hpp>
#include <nlohmann/json.hpp>

namespace neurus
{
namespace
{

using Json = nlohmann::ordered_json;

// Emit retained fields into the current cereal node, preserving JSON numbers,
// object key order, nulls and empty containers.
void writeJson(cereal::JSONOutputArchive& ar, const Json& value)
{
	switch (value.type())
	{
	case Json::value_t::object:
		ar.startNode();
		for (const auto& [key, child] : value.items())
		{
			ar.setNextName(key.c_str());
			writeJson(ar, child);
		}
		ar.finishNode();
		break;
	case Json::value_t::array:
		ar.startNode();
		ar.makeArray();
		for (const auto& child : value)
			writeJson(ar, child);
		ar.finishNode();
		break;
	case Json::value_t::string:
		ar(value.get_ref<const std::string&>());
		break;
	case Json::value_t::boolean:
		ar(value.get<bool>());
		break;
	case Json::value_t::number_unsigned:
		ar(value.get<uint64_t>());
		break;
	case Json::value_t::number_integer:
		ar(value.get<int64_t>());
		break;
	case Json::value_t::number_float:
		ar(value.get<double>());
		break;
	case Json::value_t::null:
		ar(nullptr);
		break;
	default:
		throw std::runtime_error("Unsupported JSON value in a pooled resource");
	}
}

} // namespace

void ResourceManager::save(cereal::JSONOutputArchive& ar) const
{
	ar.setNextName("resources_");
	ar.startNode();
	ar.makeArray();
	for (const int id : RegisteredIds())
	{
		if (const auto record = ArchivedRecord(id))
		{
			auto entry = Json::parse(*record);
			const auto* type = ResourceTypeRegistration::Find(entry.at("type").get_ref<const std::string&>());
			if (!type)
				throw cereal::Exception("Unknown deferred resource type for UID " + std::to_string(id));
			const auto name = RegisteredName(id);
			if (!type->name_field.empty() && !name.empty())
			{
				auto& data = entry.at("data");
				auto& fields = type->name_depth == 0 ? data : data.at("value0");
				fields[type->name_field] = name;
			}
			writeJson(ar, entry);
			continue;
		}

		const auto resource = Get(id);
		if (!resource) continue;
		const UID& object = *resource;
		const auto* type = ResourceTypeRegistration::Find(typeid(object));
		if (!type)
			throw cereal::Exception("Unregistered pooled resource type for UID " + std::to_string(id));
		ar.startNode();
		ar(cereal::make_nvp("key", id), cereal::make_nvp("type", type->name));
		type->WriteBody(ar, object);
		ar.finishNode();
	}
	ar.finishNode();
}

} // namespace neurus
