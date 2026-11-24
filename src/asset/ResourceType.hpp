#pragma once

#include <cereal/types/polymorphic.hpp>

#include <optional>
#include <string_view>
#include <typeindex>

namespace neurus
{

/// Returns the concrete type named in a pooled cereal record, without constructing it.
std::optional<std::type_index> FindResourceType(std::string_view archiveName);

/// Cereal uses the stable address of a registered name as its output-archive key.
const char* ResourceArchiveName(std::string_view archiveName);
const char* ResourceNameField(std::string_view archiveName);

/// Installed by the translation unit that registers the concrete type with cereal.
struct ResourceTypeRegistration
{
	ResourceTypeRegistration(const char* archiveName, std::type_index type, const char* nameField);

	template<class T>
	static constexpr const char* NameField()
	{
		if constexpr (requires(T object) { object.o_name; })
			return "o_name";
		if constexpr (requires(T object) { object.SetName(object.GetName()); })
			return "m_name";
		return nullptr;
	}
};

} // namespace neurus

// All pooled types live in neurus. Keep their archive names and metadata in sync.
#define NEURUS_REGISTER_RESOURCE_TYPE(Type) \
	CEREAL_REGISTER_TYPE(neurus::Type) \
	namespace { \
	const ::neurus::ResourceTypeRegistration Type##ResourceType{ \
		::cereal::detail::binding_name<::neurus::Type>::name(), typeid(::neurus::Type), \
		::neurus::ResourceTypeRegistration::NameField<::neurus::Type>()}; \
	}
