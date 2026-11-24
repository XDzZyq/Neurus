#include "core/UID.hpp"

#include <stdexcept>

namespace neurus
{

std::optional<int> UID::GetObjectID() const noexcept
{
	if (const auto membership = m_membership.lock())
		return *membership;
	return std::nullopt;
}

int UID::RequireObjectID() const
{
	if (const auto id = GetObjectID())
		return *id;
	throw std::logic_error("Object has no resource pool membership");
}

} // namespace neurus
