/**
 * @file ResourceManager.cpp
 * @brief Implementation of ResourceManager non-template members.
 */

#include "asset/ResourceManager.hpp"

#include <algorithm>
#include <limits>

namespace neurus
{

int ResourceManager::Register(std::shared_ptr<UID> obj)
{
	if (res_nextId == std::numeric_limits<int>::max())
		throw std::overflow_error("Resource UID allocator exhausted");
	return RegisterAt(res_nextId, obj);
}

int ResourceManager::RegisterAt(int id, const std::shared_ptr<UID>& object)
{
	if (!object || !object->m_membership.expired())
		throw std::runtime_error("Registration requires an unpooled object");
	const UID& resource = *object;
	IndexType(id, typeid(resource));
	try
	{
		res_live.emplace(id, object);
	}
	catch (...)
	{
		EraseResource(id);
		throw;
	}
	object->m_membership = res_entries.at(id).membership;
	return id;
}

void ResourceManager::RegisterDeferred(int id, std::type_index type, std::function<std::shared_ptr<UID>()> loader,
                                       std::shared_ptr<const std::string> archivedRecord, std::function<bool(UID&)> populate)
{
	if (!loader)
		throw std::invalid_argument("Missing resource loader for UID " + std::to_string(id));
	IndexType(id, type);
	try
	{
		res_deferred.emplace(id, DeferredResource{std::move(loader), std::move(archivedRecord), std::move(populate)});
	}
	catch (...)
	{
		EraseResource(id);
		throw;
	}
}

std::shared_ptr<UID> ResourceManager::GetLive(int id) const
{
	auto live = res_live.find(id);
	return live == res_live.end() ? nullptr : live->second;
}

std::shared_ptr<UID> ResourceManager::Get(int id) const
{
	if (auto live = GetLive(id))
		return live;
	auto deferred = res_deferred.find(id);
	if (deferred == res_deferred.end())
		return nullptr;
	auto& record = deferred->second;
	if (record.loading)
		throw std::runtime_error("Cyclic resource dependency at UID " + std::to_string(id));

	record.loading = true;
	std::shared_ptr<UID> object;
	bool attached = false;
	try
	{
		object = record.load();
		const auto& entry = res_entries.at(id);
		if (object)
		{
			const UID& resource = *object;
			if (!object->m_membership.expired() || std::type_index(typeid(resource)) != entry.type)
				throw std::runtime_error("Deferred resource has invalid membership or type");
			object->m_membership = entry.membership;
			attached = true;
		}
		if (!object || (record.populate && !record.populate(*object)))
		{
			if (!record.archivedRecord)
				throw std::runtime_error("Deferred resource could not be loaded");
			record.loading = false;
			EraseResource(id);
			return nullptr;
		}
		res_live.emplace(id, object);
		res_deferred.erase(id);
		return object;
	}
	catch (...)
	{
		if (attached) object->m_membership.reset();
		record.loading = false;
		throw;
	}
}

std::shared_ptr<UID> ResourceManager::GetByName(std::type_index type, const std::string& name) const
{
	auto names = res_namesByType.find(type);
	if (names == res_namesByType.end())
		return nullptr;

	auto entry = names->second.find(name);
	if (entry == names->second.end())
		return nullptr;

	return Get(entry->second);
}

std::string ResourceManager::SetName(int id, const std::string& name)
{
	auto metadata = res_entries.find(id);
	if (metadata == res_entries.end())
		return std::string{};

	// Drop the previous name first: renaming to the name the object already
	// owns then finds it free and stays a no-op instead of gaining a suffix.
	EraseName(id);

	const std::type_index type = metadata->second.type;
	const std::string unique = UniqueName(type, name);
	res_namesByType[type][unique] = id;
	res_nameById.insert_or_assign(id, NameEntry{type, unique});
	return unique;
}

std::string ResourceManager::RegisteredName(int id) const
{
	const auto found = res_nameById.find(id);
	return found == res_nameById.end() ? std::string{} : found->second.name;
}

std::vector<int> ResourceManager::RegisteredIds() const
{
	std::vector<int> ids;
	ids.reserve(Size());
	for (const auto& entry : res_entries)
		ids.push_back(entry.first);
	return ids;
}

std::shared_ptr<const std::string> ResourceManager::ArchivedRecord(int id) const
{
	const auto found = res_deferred.find(id);
	return found == res_deferred.end() ? nullptr : found->second.archivedRecord;
}

void ResourceManager::ForEachUID(const std::function<void(const std::shared_ptr<UID>&)>& fn) const
{
	for (int id : RegisteredIds())
		if (auto resource = Get(id))
			fn(resource);
}

void ResourceManager::ForEachRegistered(
	const std::function<void(int id, std::type_index type)>& fn) const
{
	std::vector<std::pair<int, std::type_index>> entries;
	entries.reserve(res_entries.size());
	for (const auto& [id, entry] : res_entries)
		entries.emplace_back(id, entry.type);
	std::sort(entries.begin(), entries.end(),
		[](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
	for (const auto& [id, type] : entries)
		fn(id, type);
}

void ResourceManager::ForEachOfType(
	std::type_index type,
	const std::function<void(const std::shared_ptr<UID>&)>& fn) const
{
	auto tracked = res_idsByType.find(type);
	if (tracked == res_idsByType.end())
	{
		// Base types (UID, ObjectID) are never indexed - that would duplicate
		// every id. Fall back to the pool and let the typed template filter.
		ForEachUID(fn);
		return;
	}

	const auto ids = tracked->second;
	for (int id : ids)
		if (auto resource = Get(id))
			fn(resource);
}

bool ResourceManager::Remove(int id)
{
	return EraseResource(id);
}

bool ResourceManager::EraseResource(int id) const
{
	const auto type = res_entries.find(id);
	if (type == res_entries.end())
		return false;
	const auto deferred = res_deferred.find(id);
	if (deferred != res_deferred.end() && deferred->second.loading)
		throw std::logic_error("Cannot remove a loading resource: " + std::to_string(id));

	EraseName(id);
	auto tracked = res_idsByType.find(type->second.type);
	std::erase(tracked->second, id);
	if (tracked->second.empty())
		res_idsByType.erase(tracked);
	res_entries.erase(type);
	res_deferred.erase(id);
	res_live.erase(id);
	return true;
}

void ResourceManager::Clear()
{
	for (const auto& entry : res_deferred)
		if (entry.second.loading)
			throw std::logic_error("Cannot clear the resource pool during a lookup");
	res_deferred.clear();
	res_live.clear();
	res_namesByType.clear();
	res_nameById.clear();
	res_idsByType.clear();
	res_entries.clear();
}

std::string ResourceManager::UniqueName(std::type_index type, const std::string& base) const
{
	const std::string stem = base.empty() ? std::string("Object") : base;

	auto names = res_namesByType.find(type);
	if (names == res_namesByType.end() || names->second.count(stem) == 0)
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
	if (id < 0 || id == std::numeric_limits<int>::max())
		throw std::out_of_range("Invalid resource UID");
	if (!res_entries.emplace(id, Entry{type, std::make_shared<const int>(id)}).second)
		throw std::runtime_error("Resource UID already exists: " + std::to_string(id));
	try
	{
		res_idsByType[type].push_back(id);
	}
	catch (...)
	{
		res_entries.erase(id);
		throw;
	}
	res_nextId = std::max(res_nextId, id + 1);
}

void ResourceManager::RaiseNextId(int next)
{
	if (next < 1)
		throw std::out_of_range("Invalid next resource UID");
	res_nextId = std::max(res_nextId, next);
}

void ResourceManager::EraseName(int id) const
{
	auto entry = res_nameById.find(id);
	if (entry == res_nameById.end())
		return;

	auto names = res_namesByType.find(entry->second.type);
	if (names != res_namesByType.end())
		names->second.erase(entry->second.name);
	res_nameById.erase(entry);
}

} // namespace neurus
