#include "asset/components/ResourceArchiveWriter.hpp"

#include "asset/ResourceManager.hpp"
#include "asset/ResourceType.hpp"

#include <cereal/archives/json.hpp>
#include <nlohmann/json.hpp>

namespace neurus::project
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

void writeRecord(cereal::JSONOutputArchive& ar, const std::shared_ptr<const std::string>& record,
                 const std::string& registeredName)
{
	auto document = Json::parse(*record);
	auto& value = document.at("value");
	const bool staticType = value.at("polymorphic_id") == cereal::detail::msb2_32bit;
	std::string type = "neurus::UID";
	// Exact UID pointers use cereal's static-type marker and have no name.
	if (!staticType)
	{
		type = value.at("polymorphic_name").get<std::string>();
		const char* archiveName = ResourceArchiveName(type);
		if (!archiveName)
			throw std::runtime_error("Unknown pooled resource type: " + type);
		const auto archiveId = ar.registerPolymorphicType(archiveName);
		value.at("polymorphic_id") = archiveId;
		if ((archiveId & cereal::detail::msb_32bit) == 0)
			value.erase("polymorphic_name");
	}

	auto& wrapper = value.at("ptr_wrapper");
	// A retained record represents one pooled object. Its address uniquely
	// identifies that object to this archive, just as a live shared_ptr does.
	wrapper.at("id") = ar.registerSharedPointer(record);
	if (const char* field = ResourceNameField(type); field && !registeredName.empty())
	{
		auto* base = &wrapper.at("data");
		while (!base->at("value0").contains("o_id"))
			base = &base->at("value0");
		(*base)[field] = registeredName;
	}
	writeJson(ar, value);
}

} // namespace

void WriteResourcePool(cereal::JSONOutputArchive& ar, const ResourceManager& pool)
{
	ar.setNextName("m_resources");
	ar.startNode();
	ar.setNextName("resources_");
	ar.startNode();
	ar.makeArray();
	for (const int id : pool.RegisteredIds())
	{
		ar.startNode();
		ar(cereal::make_nvp("key", id));
		ar.setNextName("value");
		if (auto record = pool.ArchivedRecord(id))
			writeRecord(ar, record, pool.RegisteredName(id));
		else
			ar(pool.Get(id));
		ar.finishNode();
	}
	ar.finishNode();
	ar.finishNode();
}

} // namespace neurus::project
