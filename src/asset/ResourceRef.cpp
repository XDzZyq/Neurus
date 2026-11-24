#include "asset/ResourceRef.hpp"

#include <stdexcept>

namespace neurus
{
namespace
{
thread_local ArchiveResourceRefs* activeRefs = nullptr;
}

ArchiveResourceRefs::ArchiveResourceRefs(const cereal::JSONInputArchive& archive)
	: m_archive(archive), m_previous(std::exchange(activeRefs, this))
{
}

ArchiveResourceRefs::~ArchiveResourceRefs()
{
	activeRefs = m_previous;
}

void ArchiveResourceRefs::Bind(const cereal::JSONInputArchive& archive, int id, Assign assign)
{
	if (!activeRefs || &archive != &activeRefs->m_archive)
		throw std::logic_error("Reading a resource reference requires a pool for this archive");
	activeRefs->m_bindings.emplace_back(id, std::move(assign));
}

void ArchiveResourceRefs::Resolve(const IResourceLookup& pool)
{
	for (const auto& [id, assign] : m_bindings)
		assign(pool.Get(id));
	m_bindings.clear();
}
} // namespace neurus
