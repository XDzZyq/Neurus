/**
 * @file ResourceManager.cpp
 * @brief Implementation of ResourceManager non-template members.
 */

#include "core/ResourceManager.hpp"

namespace neurus
{

int ResourceManager::Register(std::shared_ptr<UID> obj)
{
	if (!obj)
		throw std::runtime_error("ResourceManager::Register: null resource");

	const int id = obj->GetObjectID();
	if (Contains(id))
		throw std::runtime_error("Resource UID already exists: " + std::to_string(id));
	const UID& pooled = *obj;
	IndexType(id, typeid(pooled));
	resources_.emplace(id, std::move(obj));
	return id;
}

void ResourceManager::RegisterDeferred(int id, std::type_index type, std::function<std::shared_ptr<UID>()> loader)
{
	if (!loader)
		throw std::invalid_argument("Missing resource loader for UID " + std::to_string(id));
	if (Contains(id))
		throw std::runtime_error("Resource UID already exists: " + std::to_string(id));
	UID::ReserveThrough(id);
	IndexType(id, type);
	m_loaders.emplace(id, DeferredResource{std::move(loader)});
}

std::shared_ptr<UID> ResourceManager::Get(int id) const
{
	auto live = resources_.find(id);
	if (live != resources_.end())
		return live->second;
	auto deferred = m_loaders.find(id);
	if (deferred == m_loaders.end())
		return nullptr;
	auto& record = deferred->second;
	if (record.loading)
		throw std::runtime_error("Cyclic resource dependency at UID " + std::to_string(id));

	record.loading = true;
	try
	{
		auto object = record.load();
		if (!object || object->GetObjectID() != id)
			throw std::runtime_error("Restored resource identity mismatch for UID " + std::to_string(id));
		const UID& pooled = *object;
		if (std::type_index(typeid(pooled)) != m_typeOf.at(id))
			throw std::runtime_error("Restored resource type mismatch for UID " + std::to_string(id));
		resources_.emplace(id, object);
		m_loaders.erase(id);
		return object;
	}
	catch (...)
	{
		record.loading = false;
		throw;
	}
}

std::shared_ptr<UID> ResourceManager::GetByName(std::type_index type, const std::string& name) const
{
	auto names = m_names.find(type);
	if (names == m_names.end())
		return nullptr;

	auto entry = names->second.find(name);
	if (entry == names->second.end())
		return nullptr;

	return Get(entry->second);
}

std::string ResourceManager::SetName(int id, const std::string& name)
{
	auto metadata = m_typeOf.find(id);
	if (metadata == m_typeOf.end())
		return std::string{};

	// Drop the previous name first: renaming to the name the object already
	// owns then finds it free and stays a no-op instead of gaining a suffix.
	EraseName(id);

	const std::type_index type = metadata->second;
	const std::string unique = UniqueName(type, name);
	m_names[type][unique] = id;
	m_nameOf.insert_or_assign(id, NameEntry{type, unique});
	return unique;
}

void ResourceManager::ForEachUID(const std::function<void(const std::shared_ptr<UID>&)>& fn) const
{
	std::vector<int> ids;
	ids.reserve(Size());
	for (const auto& entry : m_typeOf)
		ids.push_back(entry.first);
	for (int id : ids)
		fn(Get(id));
}

void ResourceManager::ForEachOfType(
	std::type_index type,
	const std::function<void(const std::shared_ptr<UID>&)>& fn) const
{
	auto tracked = m_byType.find(type);
	if (tracked == m_byType.end())
	{
		// Base types (UID, ObjectID) are never indexed - that would duplicate
		// every id. Fall back to the pool and let the typed template filter.
		ForEachUID(fn);
		return;
	}

	const auto ids = tracked->second;
	for (int id : ids)
		fn(Get(id));
}

bool ResourceManager::Remove(int id)
{
	const auto type = m_typeOf.find(id);
	if (type == m_typeOf.end())
		return false;
	const auto deferred = m_loaders.find(id);
	if (deferred != m_loaders.end() && deferred->second.loading)
		throw std::logic_error("Cannot remove a loading resource: " + std::to_string(id));

	EraseName(id);
	std::erase(m_byType.at(type->second), id);
	m_typeOf.erase(type);
	m_loaders.erase(id);
	resources_.erase(id);
	return true;
}

void ResourceManager::Clear()
{
	for (const auto& entry : m_loaders)
		if (entry.second.loading)
			throw std::logic_error("Cannot clear the resource pool during a lookup");
	m_loaders.clear();
	resources_.clear();
	m_names.clear();
	m_nameOf.clear();
	m_byType.clear();
	m_typeOf.clear();
}

std::string ResourceManager::UniqueName(std::type_index type, const std::string& base) const
{
	const std::string stem = base.empty() ? std::string("Object") : base;

	auto names = m_names.find(type);
	if (names == m_names.end() || names->second.count(stem) == 0)
		return stem;

	// "Stem.001", "Stem.002", ... - the first free suffix wins.
	for (int i = 1;; ++i)
	{
		std::string index = std::to_string(i);
		if (index.size() < 3)
			index.insert(0, 3 - index.size(), '0');

		std::string candidate = stem + "." + index;
		if (names->second.count(candidate) == 0)
			return candidate;
	}
}

void ResourceManager::IndexType(int id, std::type_index type)
{
	m_typeOf.insert_or_assign(id, type);
	m_byType[type].push_back(id);
}

void ResourceManager::EraseName(int id)
{
	auto entry = m_nameOf.find(id);
	if (entry == m_nameOf.end())
		return;

	auto names = m_names.find(entry->second.type);
	if (names != m_names.end())
		names->second.erase(entry->second.name);
	m_nameOf.erase(entry);
}

void ResourceManager::RebuildTypeIndex()
{
	m_byType.clear();
	m_typeOf.clear();
	for (const auto& entry : resources_)
	{
		const UID& pooled = *entry.second;
		IndexType(entry.first, typeid(pooled));
	}
}

} // namespace neurus
