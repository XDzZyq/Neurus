#include "asset/ResourceRef.hpp"

#include <stdexcept>

namespace neurus
{
namespace
{
thread_local ArchiveResourceRefs* activeRefs = nullptr;
}

ArchiveResourceRefs::ArchiveResourceRefs(const cereal::JSONInputArchive& archive, const ResourceReadServices* services)
	: m_archive(archive), m_previous(std::exchange(activeRefs, this)), m_services(services)
{
	if (!m_services && m_previous && &m_previous->m_archive == &archive)
		m_services = m_previous->m_services;
}

ArchiveResourceRefs::~ArchiveResourceRefs()
{
	activeRefs = m_previous;
}

ArchiveResourceRefs& ArchiveResourceRefs::For(const cereal::JSONInputArchive& archive)
{
	if (!activeRefs || &archive != &activeRefs->m_archive)
		throw std::logic_error("Reading a resource reference requires a pool for this archive");
	return *activeRefs;
}

bool ArchiveResourceRefs::IsBound(const cereal::JSONInputArchive& archive)
{
	return activeRefs && &archive == &activeRefs->m_archive;
}

void ArchiveResourceRefs::Bind(const cereal::JSONInputArchive& archive, int id, Assign assign)
{
	For(archive).m_bindings.push_back({id, std::move(assign), false});
}

void ArchiveResourceRefs::BindOptional(const cereal::JSONInputArchive& archive, int id, Assign assign)
{
	For(archive).m_bindings.push_back({id, std::move(assign), true});
}

void ArchiveResourceRefs::AfterLoad(const cereal::JSONInputArchive& archive, int ownerUid, Complete complete)
{
	For(archive).m_completions.emplace_back(ownerUid, std::move(complete));
}

std::vector<int> ArchiveResourceRefs::Resolve(const IResourceLookup& pool)
{
	for (const auto& [id, assign, optional] : m_bindings)
	{
		try
		{
			assign(pool.Get(id));
		}
		catch (const std::exception& e)
		{
			if (!optional) throw;
			NEURUS_ERR("Could not restore optional reference to UID " << id << ": " << e.what());
		}
	}
	m_bindings.clear();
	std::vector<int> rejected;
	for (const auto& [uid, complete] : m_completions)
	{
		try
		{
			if (complete(pool, m_services)) continue;
		}
		catch (const std::exception& e)
		{
			NEURUS_ERR("Could not complete pooled resource UID " << uid << ": " << e.what());
		}
		rejected.push_back(uid);
	}
	m_completions.clear();
	return rejected;
}
} // namespace neurus
