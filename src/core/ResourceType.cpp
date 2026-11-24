#include "core/ResourceType.hpp"

#include <map>
#include <string>

namespace neurus
{
namespace
{

struct ResourceTypeInfo
{
	std::type_index type;
	const char* nameField;
};

auto& resourceTypes()
{
	// Keys reference static names from the corresponding cereal registration.
	static std::map<std::string_view, ResourceTypeInfo, std::less<>> types;
	return types;
}

} // namespace

ResourceTypeRegistration::ResourceTypeRegistration(const char* archiveName, std::type_index type, const char* nameField)
{
	resourceTypes().emplace(archiveName, ResourceTypeInfo{type, nameField});
}

std::optional<std::type_index> FindResourceType(std::string_view archiveName)
{
	const auto& types = resourceTypes();
	const auto it = types.find(archiveName);
	if (it == types.end())
		return std::nullopt;
	return it->second.type;
}

const char* ResourceArchiveName(std::string_view archiveName)
{
	const auto& types = resourceTypes();
	auto it = types.find(archiveName);
	return it == types.end() ? nullptr : it->first.data();
}

const char* ResourceNameField(std::string_view archiveName)
{
	const auto& types = resourceTypes();
	auto it = types.find(archiveName);
	return it == types.end() ? nullptr : it->second.nameField;
}

} // namespace neurus
