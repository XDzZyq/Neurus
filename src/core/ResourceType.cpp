#include "core/ResourceType.hpp"

#include <map>
#include <string>

namespace neurus
{
namespace
{

auto& resourceTypes()
{
	static std::map<std::string, std::type_index, std::less<>> types;
	return types;
}

} // namespace

ResourceTypeRegistration::ResourceTypeRegistration(std::string_view archiveName, std::type_index type)
{
	resourceTypes().emplace(archiveName, type);
}

std::optional<std::type_index> FindResourceType(std::string_view archiveName)
{
	const auto& types = resourceTypes();
	const auto it = types.find(archiveName);
	if (it == types.end())
		return std::nullopt;
	return it->second;
}

} // namespace neurus
