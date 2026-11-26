/**
 * @file ResourceType.hpp
 * @brief Registration metadata for pooled resource types: archive name, name field, factory and payload checks.
 */

#pragma once

#include <cereal/types/polymorphic.hpp>
#include <nlohmann/json_fwd.hpp>
#include "core/UID.hpp"

#include <cereal/archives/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <utility>

namespace neurus
{

void WriteResourceName(UID& resource, const std::string& name);
bool IsUsableResource(const UID& resource);
/// Static metadata installed by the translation unit that registers the concrete
/// type with cereal.
struct ResourceTypeRegistration
{
	using PayloadCheck = std::function<bool(const nlohmann::ordered_json&)>;
	using ObjectCheck = std::function<bool(const UID&)>;

	std::type_index type;
	std::string name;
	std::string name_field;
	int name_depth;

	std::function<void(UID&, const std::string&)> WriteName;
	std::function<std::string(const UID&)> ReadName;
	std::function<std::shared_ptr<UID>()> Create;
	std::function<void(cereal::JSONOutputArchive&, const UID&)> WriteBody;
	std::function<void(cereal::JSONInputArchive&, UID&)> ReadBody;
	PayloadCheck ValidPayload;
	ObjectCheck Usable;

	static const ResourceTypeRegistration* Find(std::string_view archive_name);
	static const ResourceTypeRegistration* Find(std::type_index type);

	/**
	 * @brief Constructor default name of the type, read from one throwaway
	 *        default-constructed object; empty when the type has no name field.
	 */
	std::string DefaultName() const;

	template<class T>
	ResourceTypeRegistration(std::type_identity<T>, std::string archive_name, int depth,
	                         PayloadCheck valid = {}, ObjectCheck check = {})
		: type(typeid(T)), name(std::move(archive_name)), name_depth(depth),
		  ValidPayload(std::move(valid)), Usable(std::move(check))
	{
		Create = []() -> std::shared_ptr<UID> { return std::make_shared<T>(); };
		WriteBody = [](cereal::JSONOutputArchive& ar, const UID& object) {
			ar(cereal::make_nvp("data", static_cast<const T&>(object)));
		};
		ReadBody = [](cereal::JSONInputArchive& ar, UID& object) {
			ar(cereal::make_nvp("data", static_cast<T&>(object)));
		};
		if constexpr (requires(T object) { object.o_name; })
		{
			name_field = "o_name";
			WriteName = [](UID& uid, const std::string& name) { static_cast<T&>(uid).o_name = name; };
			ReadName = [](const UID& uid) { return static_cast<const T&>(uid).o_name; };
		}
		else if constexpr (requires(T object) { object.SetName(object.GetName()); })
		{
			name_field = "m_name";
			WriteName = [](UID& uid, const std::string& name) { static_cast<T&>(uid).SetName(name); };
			ReadName = [](const UID& uid) { return static_cast<const T&>(uid).GetName(); };
		}
		Register();
	}

private:
	void Register() const;
};

} // namespace neurus

// All pooled types live in neurus. Keep their archive names and metadata in sync.
#define NEURUS_REGISTER_RESOURCE_TYPE(Type, ...) \
	CEREAL_REGISTER_TYPE(neurus::Type) \
	namespace { \
	const ::neurus::ResourceTypeRegistration Type##ResourceType{ \
		std::type_identity<::neurus::Type>{}, ::cereal::detail::binding_name<::neurus::Type>::name(), __VA_ARGS__}; \
	}
