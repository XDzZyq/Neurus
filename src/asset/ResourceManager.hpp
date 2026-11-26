/**
 * @file ResourceManager.hpp
 * @brief Central factory + registry for every persistent UID object.
 *
 * ResourceManager is the single factory and registry for UID-derived objects
 * (scene objects via ObjectID, data resources like MeshData/ImageData/Shader).
 * It is app-scoped, type-erased (holds shared_ptr<UID>), and owns the
 * polymorphic serialization of the whole pool.
 *
 * Design:
 * - UID is the ONLY resource base - every pooled object derives from it and
 *   serializes its own fields; no reload hook interface.
 * - Load<T>(args...) constructs + registers in one step.
 * - Scene holds typed collections. SceneComponent resolves their members by UID.
 * - No UID object knows the ResourceManager and no UID object constructs
 *   another UID object internally - construction happens through Load<T>.
 * - The pool serializes the real objects (polymorphically); Scene serializes
 *   only ID references. Pool is loaded before the Scene.
 * - Names are registry-owned: unique per concrete dynamic type, with a per-type
 *   id index so typed enumeration does not scan the pool. Load<T> registers a
 *   scene object's constructor name and mirrors the unique result into o_name.
 *
 * @note Thread-safety: Not thread-safe. Main thread only.
 */

#pragma once

#include <algorithm>
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

#include "core/IResourceLookup.hpp"
#include "asset/ResourceRef.hpp"
#include "core/UID.hpp"

namespace cereal { class JSONOutputArchive; class JSONInputArchive; }

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
	 * @brief Constructs a resource and assigns its UID through registration.
	 *
	 * When @p T exposes a public `o_name` (every scene object does, through
	 * ObjectID), the constructor's default name is registered as the object's
	 * display name and the unique result is mirrored back into `o_name`. This is
	 * the single registration point, so no creation path can forget it; a
	 * collision gains the usual ".001" suffix. Types without `o_name` (data
	 * resources) stay unnamed.
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
		const int id = Register(resource);

		// Registry-owned uniqueness, mirrored back so the Outliner, the project
		// file and name lookups agree. Duck-typed on o_name to keep core free of
		// scene headers.
		if constexpr (requires(T& t) { t.o_name; })
		{
			resource->o_name = SetName(id, resource->o_name);
		}

		return resource;
	}

	/**
	 * @brief Adopts an ephemeral object and assigns a fresh pool key.
	 * @param obj Shared pointer to the UID object.
	 * @return The registered object's UID.
	 * @throws std::runtime_error if the UID is already registered.
	 */
	int Register(std::shared_ptr<UID> obj);

	/// Next fresh key for this manager; indexing and RaiseNextId only raise it.
	int NextId() const { return res_nextId; }

	/**
	 * @brief Raises this manager's allocator to at least @p next.
	 *
	 * Never lowers it, so a stale or duplicate value cannot hand out a key that
	 * this manager already issued. Project persistence calls it after loading
	 * `next_uid`, which keeps removed or rejected keys out of later allocations.
	 *
	 * @param next Candidate next key; must be positive.
	 * @throws std::out_of_range if @p next is less than one.
	 */
	void RaiseNextId(int next);

	/**
	 * @brief Reserves a record key and exact type until Get first needs the object.
	 * The factory returns an unpooled object of the registered type. Get attaches
	 * membership before populate reads its fields and dependencies. Returning
	 * false from populate rejects an archived record and releases its name and
	 * type entries. An exception escaping either callback leaves it retryable.
	 */
	void RegisterDeferred(int id, std::type_index type, std::function<std::shared_ptr<UID>()> loader,
	                      std::shared_ptr<const std::string> archivedRecord = {},
	                      std::function<bool(UID&)> populate = {});

	/// Registry name of a live or deferred resource, or an empty string for an unnamed UID.
	std::string RegisteredName(int id) const;

	/// Snapshot registered identities for project persistence without running their loaders.
	std::vector<int> RegisteredIds() const;
	std::shared_ptr<const std::string> ArchivedRecord(int id) const;

	/// Returns a cached object without invoking a loader.
	std::shared_ptr<UID> GetLive(int id) const;

	// --- IResourceLookup ---

	using IResourceLookup::Get;
	using IResourceLookup::GetByName;
	using IResourceLookup::SetName;

	/**
	 * @brief Looks up a pooled resource by ID (base, untyped fetch).
	 * @param id Resource UID.
	 * @return Shared pointer to the UID base, or nullptr if absent.
	 */
	std::shared_ptr<UID> Get(int id) const override;

	/**
	 * @brief True if a resource with the given ID is registered.
	 */
	bool Contains(int id) const override { return res_entries.contains(id); }

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
	 * @brief Visits live objects of type @p T without running any loader.
	 *
	 * Walks the registered keys in ascending order and visits every materialized
	 * object that is a @p T or derives from it, so a deferred record stays
	 * deferred and its loader never runs. The editor's ShaderCompiled handler
	 * uses this so a deferred mesh is not loaded merely to warm a pipeline.
	 *
	 * @tparam T Type to visit (e.g. Mesh), or one of its bases.
	 * @param fn Callback receiving each live matching shared_ptr (ascending key
	 *           order).
	 * @note @p fn must not call Load/Register/Remove/Clear - mutating the pool
	 *       while it is being iterated invalidates the walk.
	 */
	template<class T>
	void ForEachLive(const std::function<void(const std::shared_ptr<T>&)>& fn) const
	{
		static_assert(std::is_base_of_v<UID, T>, "ForEachLive<T>: T must derive from UID");

		std::vector<int> ids = RegisteredIds();
		std::sort(ids.begin(), ids.end());
		for (int id : ids)
			if (auto live = std::dynamic_pointer_cast<T>(GetLive(id)))
				fn(live);
	}

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
	 *
	 * Keeps the allocator, so keys stay unique for this manager's lifetime even
	 * after a clear.
	 */
	void Clear();

	/**
	 * @brief Number of pooled resources.
	 */
	size_t Size() const { return res_entries.size(); }

	/**
	 * @brief Cereal serialization of the whole pool.
	 *
	 * Each record contains a UID key, the concrete resource type and its fields.
	 * Saving retains deferred records. ResourceComponent indexes them on load.
	 *
	 * @param ar Output archive.
	 */
	void save(cereal::JSONOutputArchive& ar) const;

private:
	/// Reverse entry for one registered name: the object's dynamic type + name.
	struct NameEntry
	{
		std::type_index type;
		std::string name;
	};

	// Main-thread-only lookup can reject a deferred record and remove it from
	// every index. Even const pool lookups require exclusive access.
	/// Per-type name -> UID registry, keyed by the object's dynamic type.
	mutable std::unordered_map<std::type_index, std::unordered_map<std::string, int>> res_namesByType;

	/// Reverse lookup: UID -> (dynamic type, registered name).
	mutable std::unordered_map<int, NameEntry> res_nameById;

	/// Per-type enumeration, so ForEach<Mesh> does not scan every UID object.
	mutable std::unordered_map<std::type_index, std::vector<int>> res_idsByType;
	struct Entry
	{
		std::type_index type;
		std::shared_ptr<const int> membership;
	};
	mutable std::unordered_map<int, Entry> res_entries;
	int res_nextId = 1;
	int RegisterAt(int id, const std::shared_ptr<UID>& object);

	struct DeferredResource
	{
		std::function<std::shared_ptr<UID>()> load;
		std::shared_ptr<const std::string> archivedRecord;
		std::function<bool(UID&)> populate;
		bool loading = false;
	};

	/// Logical-const Get materializes a deferred resource on the main thread.
	mutable std::unordered_map<int, DeferredResource> res_deferred;
	mutable std::unordered_map<int, std::shared_ptr<UID>> res_live;

	/**
	 * @brief Returns @p base or the first free "base.NNN" for @p type.
	 * @param type Concrete dynamic type whose namespace is checked.
	 * @param base Requested name; empty is treated as "Object".
	 */
	std::string UniqueName(std::type_index type, const std::string& base) const;

	/**
	 * @brief Records the exact dynamic type in the per-type index.
	 */
	void IndexType(int id, std::type_index type);

	/**
	 * @brief Drops the registered name of @p id, if any.
	 */
	void EraseName(int id) const;
	bool EraseResource(int id) const;

};

} // namespace neurus
