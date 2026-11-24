/**
 * @file UID.cpp
 * @brief Implementation of the UID base class.
 */

#include "core/UID.hpp"

#include <limits>
#include <stdexcept>

namespace neurus
{
// --- Static member definition --------------------------------------------

int UID::s_count = 0;

// --- UID -----------------------------------------------------------------

UID::UID()
{
	if (s_count == std::numeric_limits<int>::max())
		throw std::overflow_error("UID allocator exhausted");
	o_id = s_count++;
}

UID::UID(RestoredId id)
	: o_id(id.value)
{
	ReserveThrough(o_id);
}

void UID::ReserveThrough(int id)
{
	if (id < 0 || id == std::numeric_limits<int>::max())
		throw std::out_of_range("Invalid persisted UID");
	if (id >= s_count)
		s_count = id + 1;
}

} // namespace neurus
