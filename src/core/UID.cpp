/**
 * @file UID.cpp
 * @brief Implementation of UID pool membership lookup.
 */

#include "core/UID.hpp"

#include "core/Log.hpp"

namespace neurus
{

int UID::GetObjectID() const
{
	if (const auto membership = m_membership.lock())
		return *membership;
	NEURUS_ERR("Object has no resource pool membership");
	return -1;
}

} // namespace neurus
