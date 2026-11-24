#pragma once

#include <cereal/types/polymorphic.hpp>

#include <optional>
#include <string_view>
#include <typeindex>

namespace neurus
{

/// Returns the concrete type named in a pooled cereal record, without constructing it.
std::optional<std::type_index> FindResourceType(std::string_view archiveName);

/// Installed by the translation unit that registers the concrete type with cereal.
struct ResourceTypeRegistration
{
	ResourceTypeRegistration(std::string_view archiveName, std::type_index type);
};

} // namespace neurus

// All pooled types live in neurus. Keep their archive names and metadata in sync.
#define NEURUS_REGISTER_RESOURCE_TYPE(Type) \
	CEREAL_REGISTER_TYPE(neurus::Type) \
	namespace { \
	const ::neurus::ResourceTypeRegistration Type##ResourceType{ \
		"neurus::" #Type, typeid(::neurus::Type)}; \
	}
