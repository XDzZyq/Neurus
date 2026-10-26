/**
 * @file IResourceLookup.h
 * @brief Fetch/lookup plus the name registry over the UID object pool.
 *
 * Controllers depend on this narrow interface instead of the concrete
 * ResourceManager: they can fetch pooled objects by integer id, query
 * membership, enumerate them and register names, but cannot mutate the pool
 * (no Load/Register/Remove/Clear) or drive its serialization.
 *
 * The interface also exposes the pool's name registry. Names live outside UID
 * and are unique per concrete dynamic type, so a Mesh and a Camera may both be
 * called "Ground". SetName is the only way to register a name and GetByName the
 * only way to resolve one; the registry is authoritative for name lookups.
 *
 * Objects are fetched as shared_ptr because the pool owns them; a caller
 * typically derives a raw pointer from the temporary shared_ptr and uses it
 * within the handler scope (the pool keeps pooled objects alive, so id-based
 * lookup never dangles).
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <typeindex>

#include "core/UID.h"

namespace neurus
{

/**
 * @brief Fetch/lookup plus the name registry over a UID-keyed object pool.
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
	 * @brief Looks up a resource by its registered name within one concrete type.
	 * @param type Concrete dynamic type of the resource (e.g. typeid(Mesh)).
	 * @param name Registered name, unique among resources of that type.
	 * @return Shared pointer to the UID base, or nullptr if no such name.
	 */
	virtual std::shared_ptr<UID> GetByName(std::type_index type, const std::string& name) const = 0;

	/**
	 * @brief Registers (or renames) a resource and returns its unique name.
	 *
	 * Uniqueness is per concrete dynamic type: a collision is suffixed with a
	 * zero-padded counter ("Ground", "Ground.001", ...). Renaming a resource to
	 * the name it already owns is a no-op. The object is not modified - callers
	 * that also own a display name mirror the returned value.
	 *
	 * @param id Resource UID.
	 * @param name Requested name; an empty name becomes "Object".
	 * @return The unique registered name, or an empty string if @p id is absent.
	 */
	virtual std::string SetName(int id, const std::string& name) = 0;

	/**
	 * @brief Invokes @p fn on every pooled resource, untyped.
	 *
	 * Always complete, so this (plus a cast) is the base-class enumeration:
	 * `ForEach<ObjectID>` is NOT, because it keys the exact dynamic type.
	 *
	 * @param fn Callback receiving each pooled object (any order).
	 * @note @p fn must not call Load/Register/Remove/Clear - mutating the pool
	 *       while it is being iterated invalidates the walk.
	 */
	virtual void ForEachUID(const std::function<void(const std::shared_ptr<UID>&)>& fn) const = 0;

	/**
	 * @brief Invokes @p fn on every pooled resource tracked under @p type.
	 *
	 * The index keys exact dynamic types. A type with no pooled instance falls
	 * back to scanning the whole pool, so a base class is not a reliable way to
	 * enumerate derived objects: once a base instance itself is pooled, that
	 * instance owns the index entry and its derived types are skipped. Use
	 * ForEachUID for base-class enumeration and cast.
	 *
	 * @param type Concrete dynamic type to enumerate (e.g. typeid(Mesh)).
	 * @param fn Callback receiving each matching object (any order).
	 * @note @p fn must not call Load/Register/Remove/Clear - mutating the pool
	 *       while it is being iterated invalidates the walk.
	 */
	virtual void ForEachOfType(
		std::type_index type,
		const std::function<void(const std::shared_ptr<UID>&)>& fn) const = 0;

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

	/**
	 * @brief Typed name lookup: resolves a name within T's type.
	 * @tparam T Requested type (must be the registered concrete type).
	 * @param name Registered name.
	 * @return Typed shared_ptr, or nullptr if the name is unknown for T.
	 */
	template<class T>
	std::shared_ptr<T> GetByName(const std::string& name) const
	{
		return std::dynamic_pointer_cast<T>(GetByName(std::type_index(typeid(T)), name));
	}

	/**
	 * @brief Typed rename: validates the ID resolves to T, then registers it.
	 * @tparam T Requested type (must match the pooled object's concrete type).
	 * @param id Resource UID.
	 * @param name Requested name.
	 * @return The unique registered name, or an empty string when @p id does
	 *         not resolve to T.
	 */
	template<class T>
	std::string SetName(int id, const std::string& name)
	{
		return Get<T>(id) != nullptr ? SetName(id, name) : std::string{};
	}

	/**
	 * @brief Invokes @p fn on every pooled object whose exact dynamic type is T.
	 *
	 * This is not a base-class walk: once a base instance (ObjectID, UID) is
	 * itself pooled, the per-type index holds only that instance, so a derived
	 * camera/mesh/light is not visited. Enumerate a base class with ForEachUID
	 * and cast instead.
	 *
	 * @tparam T Exact dynamic type to visit (e.g. Mesh).
	 * @param fn Callback receiving each matching shared_ptr (any order).
	 * @note @p fn must not call Load/Register/Remove/Clear - mutating the pool
	 *       while it is being iterated invalidates the walk.
	 */
	template<class T>
	void ForEach(const std::function<void(const std::shared_ptr<T>&)>& fn) const
	{
		ForEachOfType(std::type_index(typeid(T)), [&fn](const std::shared_ptr<UID>& obj)
		{
			if (auto typed = std::dynamic_pointer_cast<T>(obj))
				fn(typed);
		});
	}
};

} // namespace neurus
