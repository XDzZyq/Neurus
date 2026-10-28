/**
 * @file ResourceManager.cpp
 * @brief Implementation of ResourceManager non-template members.
 */

#include "core/ResourceManager.h"

namespace neurus
{

int ResourceManager::Register(std::shared_ptr<UID> obj)
{
	if (!obj)
		throw std::runtime_error("ResourceManager::Register: null resource");

	const int id = obj->GetObjectID();
	auto [it, inserted] = resources_.emplace(id, std::move(obj));
	if (!inserted)
		throw std::runtime_error("Resource UID already exists: " + std::to_string(id));
	IndexType(it->second.get());
	return id;
}

std::shared_ptr<UID> ResourceManager::GetByName(std::type_index type, const std::string& name) const
{
	auto names = m_names.find(type);
	if (names == m_names.end())
		return nullptr;

	auto entry = names->second.find(name);
	if (entry == names->second.end())
		return nullptr;

	auto resource = resources_.find(entry->second);
	return resource == resources_.end() ? nullptr : resource->second;
}

std::string ResourceManager::SetName(int id, const std::string& name)
{
	auto resource = resources_.find(id);
	if (resource == resources_.end())
		return std::string{};

	// Drop the previous name first: renaming to the name the object already
	// owns then finds it free and stays a no-op instead of gaining a suffix.
	EraseName(id);

	const std::type_index type(typeid(*resource->second));
	const std::string unique = UniqueName(type, name);
	m_names[type][unique] = id;
	m_nameOf.insert_or_assign(id, NameEntry{type, unique});
	return unique;
}

void ResourceManager::ForEachUID(const std::function<void(const std::shared_ptr<UID>&)>& fn) const
{
	for (const auto& entry : resources_)
		fn(entry.second);
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
		for (const auto& entry : resources_)
			fn(entry.second);
		return;
	}

	for (int id : tracked->second)
	{
		auto resource = resources_.find(id);
		if (resource != resources_.end())
			fn(resource->second);
	}
}

bool ResourceManager::Remove(int id)
{
	auto resource = resources_.find(id);
	if (resource == resources_.end())
		return false;

	EraseName(id);

	auto type = m_typeOf.find(id);
	if (type != m_typeOf.end())
	{
		auto tracked = m_byType.find(type->second);
		if (tracked != m_byType.end())
			std::erase(tracked->second, id);
		m_typeOf.erase(type);
	}

	resources_.erase(resource);
	return true;
}

void ResourceManager::Clear()
{
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

void ResourceManager::IndexType(UID* obj)
{
	const std::type_index type(typeid(*obj));
	const int id = obj->GetObjectID();
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
		IndexType(entry.second.get());
}

} // namespace neurus
