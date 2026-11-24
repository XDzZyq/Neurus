/**
 * @file UID.cpp
 * @brief Implementation of the UID base class.
 */

#include "core/UID.hpp"
#include "core/ArchiveUid.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace neurus
{
namespace
{
thread_local int archiveId = -1;
}

ArchiveUid::ArchiveUid(int id)
	: m_previous(std::exchange(archiveId, id))
{
}

ArchiveUid::~ArchiveUid()
{
	archiveId = m_previous;
}

// --- Static member definition --------------------------------------------

int UID::s_count = 0;

// --- UID -----------------------------------------------------------------

UID::UID()
	: o_id(std::exchange(archiveId, -1))
{
	if (o_id >= 0)
		return;
	if (s_count == std::numeric_limits<int>::max())
		throw std::overflow_error("UID allocator exhausted");
	o_id = s_count++;
}

void UID::ReserveThrough(int id)
{
	if (id < 0 || id == std::numeric_limits<int>::max())
		throw std::out_of_range("Invalid persisted UID");
	if (id >= s_count)
		s_count = id + 1;
}

} // namespace neurus
