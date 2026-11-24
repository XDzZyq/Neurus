#include "asset/ResourceType.hpp"

#include <map>
#include <string>

namespace neurus
{
namespace
{

auto& resourceTypes()
{
	// Keys reference static names from the corresponding cereal registration.
	static std::map<std::string_view, const ResourceTypeRegistration*, std::less<>> types;
	return types;
}

auto& liveTypes()
{
	static std::map<std::type_index, const ResourceTypeRegistration*> types;
	return types;
}

const ResourceTypeRegistration* findLiveType(const UID& resource)
{
	const auto& types = liveTypes();
	const auto it = types.find(typeid(resource));
	return it == types.end() ? nullptr : it->second;
}

} // namespace

void ResourceTypeRegistration::Register(const char* archiveName) const
{
	resourceTypes().emplace(archiveName, this);
	liveTypes().emplace(type, this);
}

std::optional<std::type_index> FindResourceType(std::string_view archiveName)
{
	const auto& types = resourceTypes();
	const auto it = types.find(archiveName);
	if (it == types.end())
		return std::nullopt;
	return it->second->type;
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
	return it == types.end() ? nullptr : it->second->nameField;
}

int ResourceIdentityDepth(std::string_view archiveName)
{
	return resourceTypes().at(archiveName)->identityDepth;
}

std::string ReadResourceName(const UID& resource)
{
	const auto* info = findLiveType(resource);
	return info && info->readName ? info->readName(resource) : std::string{};
}

void WriteResourceName(UID& resource, const std::string& name)
{
	if (const auto* info = findLiveType(resource); info && info->writeName)
		info->writeName(resource, name);
}

bool IsUsableResource(const UID& resource)
{
	const auto* info = findLiveType(resource);
	return !info || !info->usable || info->usable(resource);
}

bool ValidResourcePayload(std::string_view archiveName, const nlohmann::ordered_json& data)
{
	const auto check = resourceTypes().at(archiveName)->validPayload;
	return !check || check(data);
}

} // namespace neurus
