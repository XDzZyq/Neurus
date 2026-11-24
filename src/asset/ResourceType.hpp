#pragma once

#include <cereal/types/polymorphic.hpp>
#include <nlohmann/json_fwd.hpp>
#include "core/UID.hpp"

#include <cereal/archives/json.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>

namespace neurus
{

std::string ReadResourceName(const UID& resource);
void WriteResourceName(UID& resource, const std::string& name);
bool IsUsableResource(const UID& resource);
/// Static metadata installed by the translation unit that registers the concrete
/// type with cereal.
struct ResourceTypeRegistration
{
	using PayloadCheck = bool (*)(const nlohmann::ordered_json&);
	using ObjectCheck = bool (*)(const UID&);

	std::type_index type;
	const char* name;
	int nameDepth;
	void (*writeBody)(cereal::JSONOutputArchive&, const UID&);
	std::shared_ptr<UID> (*create)();
	void (*readBody)(cereal::JSONInputArchive&, UID&);
	static const ResourceTypeRegistration* Find(std::string_view archiveName);
	static const ResourceTypeRegistration* Find(std::type_index type);
	PayloadCheck validPayload;
	ObjectCheck usable;
	const char* nameField = nullptr;
	std::string (*readName)(const UID&) = nullptr;
	void (*writeName)(UID&, const std::string&) = nullptr;

	template<class T>
	ResourceTypeRegistration(std::type_identity<T>, const char* archiveName, int nameDepth,
	                         PayloadCheck valid = nullptr, ObjectCheck check = nullptr)
		: type(typeid(T)), name(archiveName), nameDepth(nameDepth), validPayload(valid), usable(check)
	{
		create = []() -> std::shared_ptr<UID> { return std::make_shared<T>(); };
		writeBody = [](cereal::JSONOutputArchive& ar, const UID& object) {
			ar(cereal::make_nvp("data", static_cast<const T&>(object)));
		};
		readBody = [](cereal::JSONInputArchive& ar, UID& object) {
			ar(cereal::make_nvp("data", static_cast<T&>(object)));
		};
		if constexpr (requires(T object) { object.o_name; })
		{
			nameField = "o_name";
			readName = [](const UID& uid) { return static_cast<const T&>(uid).o_name; };
			writeName = [](UID& uid, const std::string& name) { static_cast<T&>(uid).o_name = name; };
		}
		else if constexpr (requires(T object) { object.SetName(object.GetName()); })
		{
			nameField = "m_name";
			readName = [](const UID& uid) { return static_cast<const T&>(uid).GetName(); };
			writeName = [](UID& uid, const std::string& name) { static_cast<T&>(uid).SetName(name); };
		}
		Register(archiveName);
	}

private:
	void Register(const char* archiveName) const;
};

} // namespace neurus

// All pooled types live in neurus. Keep their archive names and metadata in sync.
#define NEURUS_REGISTER_RESOURCE_TYPE(Type, ...) \
	CEREAL_REGISTER_TYPE(neurus::Type) \
	namespace { \
	const ::neurus::ResourceTypeRegistration Type##ResourceType{ \
		std::type_identity<::neurus::Type>{}, ::cereal::detail::binding_name<::neurus::Type>::name(), __VA_ARGS__}; \
	}
