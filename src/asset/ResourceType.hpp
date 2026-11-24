#pragma once

#include <cereal/types/polymorphic.hpp>
#include <nlohmann/json_fwd.hpp>
#include "core/UID.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>

namespace neurus
{

/// Returns the concrete type named in a pooled cereal record, without constructing it.
std::optional<std::type_index> FindResourceType(std::string_view archiveName);

/// Cereal uses the stable address of a registered name as its output-archive key.
const char* ResourceArchiveName(std::string_view archiveName);
const char* ResourceNameField(std::string_view archiveName);
int ResourceIdentityDepth(std::string_view archiveName);
std::string ReadResourceName(const UID& resource);
void WriteResourceName(UID& resource, const std::string& name);
bool IsUsableResource(const UID& resource);
bool ValidResourcePayload(std::string_view archiveName, const nlohmann::ordered_json& data);

/// Static metadata installed by the translation unit that registers the concrete
/// type with cereal. Depth counts unnamed serialized bases down to UID.
struct ResourceTypeRegistration
{
	using PayloadCheck = bool (*)(const nlohmann::ordered_json&);
	using ObjectCheck = bool (*)(const UID&);

	std::type_index type;
	int identityDepth;
	PayloadCheck validPayload;
	ObjectCheck usable;
	const char* nameField = nullptr;
	std::string (*readName)(const UID&) = nullptr;
	void (*writeName)(UID&, const std::string&) = nullptr;

	template<class T>
	ResourceTypeRegistration(std::type_identity<T>, const char* archiveName, int depth,
	                         PayloadCheck valid = nullptr, ObjectCheck check = nullptr)
		: type(typeid(T)), identityDepth(depth), validPayload(valid), usable(check)
	{
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
