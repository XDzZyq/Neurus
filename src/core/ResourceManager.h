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
#include <unordered_map>
#include <utility>

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
 * Implements IResourceLookup (fetch/lookup only) so controllers can resolve
 * object references by integer id without depending on the concrete pool.
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
	 * @brief Looks up a pooled resource by ID with type checking.
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
	 * @brief Invokes @p fn on every pooled object of type T.
	 *
	 * Used to mirror the pool into UID-keyed GPU caches, which follow the pool
	 * rather than the scene (pooled objects stay referenced by undo history
	 * after deletion).
	 *
	 * @tparam T Requested type (e.g. Mesh, Light).
	 * @param fn Callback receiving each matching shared_ptr (any order).
	 */
	template<class T>
	void ForEach(const std::function<void(const std::shared_ptr<T>&)>& fn) const
	{
		for (const auto& [id, obj] : resources_)
		{
			auto typed = std::dynamic_pointer_cast<T>(obj);
			if (typed)
				fn(typed);
		}
	}

	/**
	 * @brief Removes a resource by ID.
	 * @return True if an entry was removed.
	 */
	bool Remove(int id) { return resources_.erase(id) != 0; }

	/**
	 * @brief Drops every pooled object (project new/open).
	 */
	void Clear() { resources_.clear(); }

	/**
	 * @brief Number of pooled resources.
	 */
	size_t Size() const { return resources_.size(); }

	/**
	 * @brief Cereal serialization of the whole pool.
	 *
	 * Save writes every pooled UID object polymorphically; load restores them,
	 * and each object's own serialize(load) restores its content.
	 *
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(resources_));
	}

private:
	/// Pool of every registered UID object, keyed by UID.
	std::unordered_map<int, std::shared_ptr<UID>> resources_;
};

} // namespace neurus
