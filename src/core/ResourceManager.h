/**
 * @file ResourceManager.h
 * @brief Central factory + registry for every persistent UID object.
 *
 * ResourceManager is the single factory and registry for UID-derived objects
 * (scene objects via ObjectID, data resources like MeshData/ImageData/Shader).
 * It is app-scoped, type-erased (holds shared_ptr<UID>), and owns the
 * polymorphic serialization of the whole pool.
 *
 * Design:
 * - UID is the ONLY resource base - every pooled object derives from it and
 *   serializes itself (identity + own fields); no reload hook interface.
 * - Load<T>(args...) constructs + registers in one step.
 * - Scene neither owns nor references the pool; it holds plain typed pools and
 *   resolves references against the pool via Scene::ResolveReferences.
 * - No UID object knows the ResourceManager and no UID object constructs
 *   another UID object internally - construction happens through Load<T>.
 * - The pool serializes the real objects (polymorphically); Scene serializes
 *   only ID references. Pool is loaded before the Scene.
 * - Names are registry-owned, not object state: unique per concrete dynamic
 *   type, with a per-type id index so typed enumeration does not scan the pool.
 *
 * @note Thread-safety: Not thread-safe. Main thread only.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/unordered_map.hpp>

#include "core/IResourceLookup.h"
#include "core/UID.h"

namespace neurus
{

/**
 * @brief Single factory + registry for all UID objects (scene + data).
 *
 * Cleared on project new/open so no stale object leaks into a save.
 * Implements IResourceLookup (fetch/lookup, name registry, per-type
 * enumeration) so controllers can resolve object references without depending
 * on the concrete pool.
 *
 * Per-type enumeration keys the EXACT dynamic type: ForEach<T> is not a
 * base-class walk. Base enumeration (ObjectID, UID) must use ForEachUID and
 * cast, or a pooled base instance silently hides every derived object.
 */
class ResourceManager : public IResourceLookup
{
public:
	ResourceManager() = default;
	~ResourceManager() = default;

	ResourceManager(const ResourceManager&) = delete;
	ResourceManager& operator=(const ResourceManager&) = delete;

	/**
	 * @brief Constructs a resource and registers it by its UID.
	 *
	 * @tparam T Resource type (must inherit UID, directly or transitively).
	 * @tparam Args Constructor argument types.
	 * @param args Forwarded to T's constructor.
	 * @return Typed shared_ptr to the registered resource.
	 * @throws std::runtime_error if the UID is already registered (safety net;
	 *         unreachable in normal flow because UIDs are monotonic).
	 */
	template<typename T, typename... Args>
	std::shared_ptr<T> Load(Args&&... args)
	{
		static_assert(std::is_base_of_v<UID, T>, "Load<T>: T must derive from UID");

		auto resource = std::make_shared<T>(std::forward<Args>(args)...);
		const int id = resource->GetObjectID();
		auto [it, inserted] = resources_.emplace(id, std::move(resource));
		if (!inserted)
		{
			throw std::runtime_error("Resource UID already exists: " + std::to_string(id));
		}
		IndexType(it->second.get());

		return std::dynamic_pointer_cast<T>(it->second);
	}

	/**
	 * @brief Registers an already-constructed object (tests, legacy paths).
	 * @param obj Shared pointer to the UID object.
	 * @return The registered object's UID.
	 * @throws std::runtime_error if the UID is already registered.
	 */
	int Register(std::shared_ptr<UID> obj);

	// --- IResourceLookup ---

	using IResourceLookup::Get;
	using IResourceLookup::GetByName;
	using IResourceLookup::SetName;

	/**
	 * @brief Looks up a pooled resource by ID (base, untyped fetch).
	 * @param id Resource UID.
	 * @return Shared pointer to the UID base, or nullptr if absent.
	 */
	std::shared_ptr<UID> Get(int id) const override
	{
		auto it = resources_.find(id);
		if (it == resources_.end())
			return nullptr;
		return it->second;
	}

	/**
	 * @brief True if a resource with the given ID is registered.
	 */
	bool Contains(int id) const override { return resources_.count(id) != 0; }

	/**
	 * @brief Looks up a resource by its registered name within one concrete type.
	 * @param type Concrete dynamic type of the resource (e.g. typeid(Mesh)).
	 * @param name Registered name, unique among resources of that type.
	 * @return Shared pointer to the UID base, or nullptr if no such name.
	 */
	std::shared_ptr<UID> GetByName(std::type_index type, const std::string& name) const override;

	/**
	 * @brief Registers (or renames) a resource and returns its unique name.
	 *
	 * Names are unique per concrete dynamic type; a collision is suffixed with a
	 * zero-padded counter. Renaming to the name the resource already owns is a
	 * no-op. The object itself is never modified.
	 *
	 * @param id Resource UID.
	 * @param name Requested name; an empty name becomes "Object".
	 * @return The unique registered name, or an empty string if @p id is absent.
	 */
	std::string SetName(int id, const std::string& name) override;

	/**
	 * @brief Invokes @p fn on every pooled resource, untyped.
	 *
	 * Always complete; this plus a cast is the base-class enumeration.
	 *
	 * @param fn Callback receiving each pooled object (any order).
	 * @note @p fn must not call Load/Register/Remove/Clear - mutating the pool
	 *       while it is being iterated invalidates the walk.
	 */
	void ForEachUID(const std::function<void(const std::shared_ptr<UID>&)>& fn) const override;

	/**
	 * @brief Invokes @p fn on every pooled resource tracked under @p type.
	 *
	 * The index keys exact dynamic types, so a base class is not a base-class
	 * walk: a pooled base instance owns the index entry and its derived types
	 * are skipped. A type with no instance at all falls back to the whole pool.
	 * Use ForEachUID for base enumeration.
	 *
	 * @param type Concrete dynamic type to enumerate (e.g. typeid(Mesh)).
	 * @param fn Callback receiving each matching object (any order).
	 * @note @p fn must not call Load/Register/Remove/Clear - mutating the pool
	 *       while it is being iterated invalidates the walk.
	 */
	void ForEachOfType(
		std::type_index type,
		const std::function<void(const std::shared_ptr<UID>&)>& fn) const override;

	/**
	 * @brief Removes a resource by ID.
	 *
	 * Frees the registered name and the per-type index entry with the object, so
	 * a later resource can reuse the name.
	 *
	 * @return True if an entry was removed.
	 */
	bool Remove(int id);

	/**
	 * @brief Drops every pooled object and its name/type indexes (project new/open).
	 */
	void Clear();

	/**
	 * @brief Number of pooled resources.
	 */
	size_t Size() const { return resources_.size(); }

	/**
	 * @brief Cereal serialization of the whole pool.
	 *
	 * Save writes every pooled UID object polymorphically; load restores them,
	 * and each object's own serialize(load) restores its content. Names and the
	 * per-type index are derived state, never serialized: a load replaces the
	 * pool, so it drops the old registry, rebuilds the type index from the
	 * restored objects, and leaves names for the caller to re-seed.
	 *
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(resources_));
		if constexpr (Archive::is_loading::value)
		{
			// Not derived state a rebuild can recover: names are only in the
			// registry, so an old entry would survive its object and keep
			// stealing the bare name from a later resource.
			m_names.clear();
			m_nameOf.clear();
			RebuildTypeIndex();
		}
	}

private:
	/// Reverse entry for one registered name: the object's dynamic type + name.
	struct NameEntry
	{
		std::type_index type;
		std::string name;
	};

	/// Per-type name -> UID registry, keyed by the object's dynamic type.
	std::unordered_map<std::type_index, std::unordered_map<std::string, int>> m_names;

	/// Reverse lookup: UID -> (dynamic type, registered name).
	std::unordered_map<int, NameEntry> m_nameOf;

	/// Per-type enumeration, so ForEach<Mesh> does not scan every UID object.
	std::unordered_map<std::type_index, std::vector<int>> m_byType;
	std::unordered_map<int, std::type_index> m_typeOf;

	/// Pool of every registered UID object, keyed by UID.
	std::unordered_map<int, std::shared_ptr<UID>> resources_;

	/**
	 * @brief Returns @p base or the first free "base.NNN" for @p type.
	 * @param type Concrete dynamic type whose namespace is checked.
	 * @param base Requested name; empty is treated as "Object".
	 */
	std::string UniqueName(std::type_index type, const std::string& base) const;

	/**
	 * @brief Records @p obj's dynamic type in the per-type index.
	 */
	void IndexType(UID* obj);

	/**
	 * @brief Drops the registered name of @p id, if any.
	 */
	void EraseName(int id);

	/**
	 * @brief Rebuilds the per-type index from the current pool (after load).
	 */
	void RebuildTypeIndex();
};

} // namespace neurus
