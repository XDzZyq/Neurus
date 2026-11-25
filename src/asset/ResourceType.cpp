#include "asset/ResourceType.hpp"

#include <map>
#include <string>

namespace neurus
{
namespace
{

auto& resourceTypes()
{
	static std::map<std::string, const ResourceTypeRegistration*, std::less<>> types;
	return types;
}

auto& liveTypes()
{
	static std::map<std::type_index, const ResourceTypeRegistration*> types;
	return types;
}

} // namespace

void ResourceTypeRegistration::Register() const
{
	resourceTypes().emplace(name, this);
	liveTypes().emplace(type, this);
}

const ResourceTypeRegistration* ResourceTypeRegistration::Find(std::string_view name)
{
	const auto it = resourceTypes().find(name);
	return it == resourceTypes().end() ? nullptr : it->second;
}

const ResourceTypeRegistration* ResourceTypeRegistration::Find(std::type_index type)
{
	const auto it = liveTypes().find(type);
	return it == liveTypes().end() ? nullptr : it->second;
}

void WriteResourceName(UID& resource, const std::string& name)
{
	if (const auto* info = ResourceTypeRegistration::Find(typeid(resource)); info && info->WriteName)
		info->WriteName(resource, name);
}

bool IsUsableResource(const UID& resource)
{
	const auto* info = ResourceTypeRegistration::Find(typeid(resource));
	return !info || !info->Usable || info->Usable(resource);
}

} // namespace neurus
