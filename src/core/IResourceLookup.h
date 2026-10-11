/**
 * @file IResourceLookup.h
 * @brief Read-only fetch/lookup interface for the UID object pool.
 *
 * Controllers depend on this narrow interface instead of the concrete
 * ResourceManager: they can fetch pooled objects by integer id (and query
 * membership) but cannot mutate the pool or drive its serialization.
 *
 * Objects are fetched as shared_ptr because the pool owns them; a caller
 * typically derives a raw pointer from the temporary shared_ptr and uses it
 * within the handler scope (the pool keeps pooled objects alive, so id-based
 * lookup never dangles).
 */

#pragma once

#include <memory>

#include "core/UID.h"

namespace neurus {

/**
 * @brief Read-only fetch/lookup over a UID-keyed object pool.
 */
class IResourceLookup
{
public:
	virtual ~IResourceLookup() = default;

	/**
	 * @brief Looks up a pooled resource by ID (base, untyped fetch).
	 * @param id Resource UID.
	 * @return Shared pointer to the UID base, or nullptr if absent.
	 */
	virtual std::shared_ptr<UID> Get(int id) const = 0;

	/**
	 * @brief True if a resource with the given ID is registered.
	 */
	virtual bool Contains(int id) const = 0;

	/**
	 * @brief Typed lookup: fetches by ID and casts to T.
	 * @tparam T Requested type (may be a base like UID/ObjectID).
	 * @param id Resource UID.
	 * @return Typed shared_ptr, or nullptr if absent or of a different type.
	 */
	template<class T>
	std::shared_ptr<T> Get(int id) const
	{
		return std::dynamic_pointer_cast<T>(Get(id));
	}
};

} // namespace neurus
