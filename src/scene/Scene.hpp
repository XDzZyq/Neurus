/**
 * @file Scene.hpp
 * @brief Scene container holding all scene objects and scene-wide state.
 *
 * Provides the Scene class which aggregates cameras, lights, meshes,
 * sprites, debug primitives and other objects into a unified scene graph via
 * typed object pools (ResPool).
 *
 * @note Inheritance: Scene inherits UID for a unique scene identifier.
 * @note Not thread-safe. Access from main thread only.
 */

#pragma once

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "scene/ObjectID.hpp"
#include "asset/ResourceRef.hpp"
#include "core/Selections.hpp"

#include "Camera.hpp"
#include "DebugLine.hpp"
#include "DebugMesh.hpp"
#include "DebugPoints.hpp"
#include "Light.hpp"
#include "Mesh.hpp"
#include "Sprite.hpp"
#include "Environment.hpp"

namespace neurus
{

class Scene;

/**
 * @brief Snapshots a pointer-based selection set as absolute object UIDs.
 * @param selections Runtime selection holding const ObjectID* pointers.
 * @param outSelectedUids Filled with the selected objects' UIDs (order preserved).
 * @param outActiveUid Set to the active object's UID, or 0 if none.
 */
inline void SnapshotSelectionUids(const Selections<const ObjectID*>& selections,
                                  std::vector<int>& outSelectedUids, int& outActiveUid);

/**
 * @brief Restores a selection set from UIDs, resolving them against a scene.
 * @param scene Scene used to resolve UIDs to live objects.
 * @param selectedUids Object UIDs to select (stale UIDs are skipped).
 * @param activeUid Active object UID, or 0 for none.
 */
inline void RestoreSelectionUids(Scene& scene, const std::vector<int>& selectedUids, int activeUid);

/**
 * @brief Container for all scene objects, environments, and scene-wide state.
 *
 * Provides typed pools (ResPool) per object category for efficient iteration
 * and lookup by UID; active objects (camera, environment) are cached. Scene
 * modification status tracks which aspects changed since the last check.
 *
 * @note Not thread-safe. Access from main thread only.
 */
class Scene : public UID
{
public:
	// Bring base class GetObjectID() into scope alongside GetObjectID(int)
	using UID::GetObjectID;
	/**
	 * @brief Shared ownership wrapper for scene objects.
	 * @tparam Object Scene object type (Camera, Mesh, Light, etc.)
	 */
	template<class Object>
	using Resource = ResourceRef<Object>;

	/**
	 * @brief Typed object pool indexed by unique object ID.
	 *
	 * Maps an object's UID to its pooled reference, enabling O(1) lookup and removal.
	 *
	 * @tparam Object Scene object type stored in this pool.
	 */
	template<class Object>
	using ResPool = std::unordered_map<int, Resource<Object>>;

	// -------------------------------------------------------------------
	// SceneModifStatus - change tracking bitfield
	// -------------------------------------------------------------------

	/**
	 * @brief Scene modification status flags for change tracking.
	 *
	 * Tracks which aspects of the scene changed since the last check; flags
	 * are set on change and cleared after processing.
	 */
	enum SceneModifStatus
	{
		NoChanges          = 0,                             ///< No modifications since last check
		ObjectTransChanged = 1 << 0,                        ///< Object transforms updated
		LightChanged       = 1 << 1,                        ///< Light properties changed
		CameraChanged      = 1 << 2,                        ///< Camera properties changed
		ShaderChanged      = 1 << 3,                        ///< Shaders recompiled
		MaterialChanged    = 1 << 4,                        ///< Material properties changed

		/** Any core scene content changed (transforms, lights, camera, shaders, materials). */
		SceneChanged = ObjectTransChanged | LightChanged | CameraChanged | ShaderChanged | MaterialChanged,

		SDFChanged         = 1 << 8,                        ///< SDF field requires rebuild
	};

	// -------------------------------------------------------------------
	// Construction / destruction
	// -------------------------------------------------------------------

	/** @brief Constructs an empty scene. */
	Scene();

	/** @brief Destroys the scene and all owned objects. */
	~Scene();

	// -------------------------------------------------------------------
	// Status tracking
	// -------------------------------------------------------------------

	/**
	 * @brief Sets or clears scene status flag(s).
	 * @param tar Flag(s) to modify (combine with bitwise OR).
	 * @param value True sets the flag(s), false clears them.
	 */
	void UpdateSceneStatus(int tar, bool value);

	/**
	 * @brief Directly replaces the scene status bitfield.
	 * @param tar New status value.
	 * @param value Unused (legacy parameter, kept for API compatibility).
	 */
	void SetSceneStatus(int tar, bool value);

	/**
	 * @brief Checks if any of the given status flags is set.
	 * @param tar Status flag(s) to check.
	 * @return True if any of the specified flags are set.
	 */
	bool CheckStatus(SceneModifStatus tar);

	/** @brief Resets scene status to NoChanges. */
	void ResetStatus();

	// -------------------------------------------------------------------
	// Object pools
	// -------------------------------------------------------------------

	ResPool<ObjectID>    obj_list;      ///< All scene objects (base type, master pool)
	ResPool<Camera>      cam_list;      ///< Camera objects
	ResPool<Mesh>        mesh_list;     ///< Mesh objects
	ResPool<Light>       light_list;    ///< Light objects (point, sun, spot, area)
	ResPool<Sprite>      sprite_list;   ///< 2D sprite overlays
	ResPool<DebugLine>   dLine_list;    ///< Debug line primitives
	ResPool<DebugPoints> dPoints_list;  ///< Debug point primitives
	ResPool<DebugMesh>   dMesh_list;    ///< Debug wireframe-mesh primitives
	ResPool<Environment> env_list;      ///< Environment objects (IBL)

	/// Selection state. Persisted as UIDs by project persistence and resolved to
	/// pointers on load.
	Selections<const ObjectID*> selections;

	/// Clears membership, selection and camera activation before replacing scene content.
	void ClearObjects();

	// -------------------------------------------------------------------
	// Registration - store in both type-specific pool AND obj_list
	// -------------------------------------------------------------------

	/**
	 * @brief Registers a camera in the scene (cam_list and obj_list).
	 * @param camera Shared pointer to Camera object.
	 */
	void UseCamera(Resource<Camera> camera)
	{
		RegisterObject(camera, cam_list);
	}

	// -------------------------------------------------------------------
	// Camera activation - which scene camera the viewport looks through
	// -------------------------------------------------------------------

	/**
	 * @brief Activates a scene camera as the one the viewport uses.
	 *
	 * Activation is scene state, independent of selection; with no camera
	 * activated the viewport falls back to its own camera.
	 *
	 * @param uid Camera UID; must already be registered via UseCamera().
	 * @return false when uid is not in cam_list.
	 */
	bool ActivateCamera(int uid);

	/// @brief Clears the activation; the viewport falls back to its own camera.
	void DeactivateCamera() { m_activeCamUid = 0; }

	/**
	 * @brief UID of the activated camera, or 0 when none is activated.
	 *
	 * @warning May name a camera that is not currently in cam_list: the UID is
	 *          deliberately left stale when a camera is removed so that undoing
	 *          the removal restores the activation for free. Use it only for
	 *          UID comparison; callers that need a camera must go through
	 *          GetActiveCamera(), which validates against cam_list.
	 */
	int ActiveCameraID() const { return m_activeCamUid; }

	/// Restores the document UID, including stale references needed by undo.
	void RestoreActiveCameraID(int uid) { m_activeCamUid = uid; }

	/**
	 * @brief Registers a mesh in the scene (mesh_list and obj_list).
	 * @param mesh Shared pointer to Mesh object.
	 */
	void UseMesh(Resource<Mesh> mesh)
	{
		RegisterObject(mesh, mesh_list);
	}

	/**
	 * @brief Registers a light in the scene (light_list and obj_list).
	 * @param light Shared pointer to Light object.
	 */
	void UseLight(Resource<Light> light)
	{
		RegisterObject(light, light_list);
	}

	/**
	 * @brief Registers a sprite in the scene (sprite_list and obj_list).
	 * @param sprite Shared pointer to Sprite object.
	 */
	void UseSprite(Resource<Sprite> sprite)
	{
		RegisterObject(sprite, sprite_list);
	}

	/**
	 * @brief Registers debug lines in the scene (dLine_list and obj_list).
	 * @param dline Shared pointer to DebugLine object.
	 */
	void UseDebugLine(Resource<DebugLine> dline)
	{
		RegisterObject(dline, dLine_list);
	}

	/**
	 * @brief Registers debug points in the scene (dPoints_list and obj_list).
	 * @param dpoints Shared pointer to DebugPoints object.
	 */
	void UseDebugPoints(Resource<DebugPoints> dpoints)
	{
		RegisterObject(dpoints, dPoints_list);
	}

	/**
	 * @brief Registers a debug wireframe mesh in the scene (dMesh_list and obj_list).
	 * @param dmesh Shared pointer to DebugMesh object.
	 */
	void UseDebugMesh(Resource<DebugMesh> dmesh)
	{
		RegisterObject(dmesh, dMesh_list);
	}

	/**
	 * @brief Registers an environment object in the scene (env_list and obj_list).
	 * @param env Shared pointer to Environment object.
	 */
	void UseEnvironment(Resource<Environment> env)
	{
		RegisterObject(env, env_list);
	}

	/**
	 * @brief Unregisters an object from its type pool and obj_list.
	 * @tparam T Object type (Camera, Mesh, Light, Environment, ...).
	 * @param id Object UID.
	 * @param typePool Type-specific pool to remove from.
	 * @return true if the object was registered under this type.
	 * @note Also clears the object from the selection; the pooled resource
	 *       itself stays in the ResourceManager, so undo can re-register it.
	 */
	template<typename T>
	bool RemoveObject(int id, ResPool<T>& typePool)
	{
		auto it = typePool.find(id);
		if (it == typePool.end())
			return false; // not registered as this type — stale UID is a safe no-op
		auto oit = obj_list.find(id);
		if (oit != obj_list.end())
			selections.Deselect(oit->second.get(), false);
		typePool.erase(id);
		obj_list.erase(id);
		return true;
	}

	/**
	 * @brief Removes a camera's scene reference.
	 * @param id Camera UID.
	 * @return true if a camera with that UID was registered.
	 */
	bool RemoveCamera(int id) { return RemoveObject(id, cam_list); }

	/**
	 * @brief Removes a mesh's scene reference.
	 * @param id Mesh UID.
	 * @return true if a mesh with that UID was registered.
	 */
	bool RemoveMesh(int id) { return RemoveObject(id, mesh_list); }

	/**
	 * @brief Removes a light's scene reference.
	 * @param id Light UID.
	 * @return true if a light with that UID was registered.
	 */
	bool RemoveLight(int id) { return RemoveObject(id, light_list); }

	/**
	 * @brief Removes an environment's scene reference.
	 * @param id Environment UID.
	 * @return true if an environment with that UID was registered.
	 */
	bool RemoveEnvironment(int id) { return RemoveObject(id, env_list); }

	/**
	 * @brief Removes a debug line's scene reference.
	 * @param id DebugLine UID.
	 * @return true if a debug line with that UID was registered.
	 */
	bool RemoveDebugLine(int id) { return RemoveObject(id, dLine_list); }

	/**
	 * @brief Removes a debug point set's scene reference.
	 * @param id DebugPoints UID.
	 * @return true if a debug point set with that UID was registered.
	 */
	bool RemoveDebugPoints(int id) { return RemoveObject(id, dPoints_list); }

	/**
	 * @brief Removes a debug wireframe mesh's scene reference.
	 * @param id DebugMesh UID.
	 * @return true if a debug mesh with that UID was registered.
	 */
	bool RemoveDebugMesh(int id) { return RemoveObject(id, dMesh_list); }

	// -------------------------------------------------------------------
	// Lookup
	// -------------------------------------------------------------------

	/**
	 * @brief Retrieves an object by its unique ID from the master pool.
	 * @param id Unique object identifier (from UID::GetObjectID()).
	 * @return Non-owning pointer to ObjectID, or nullptr if not found.
	 */
	ObjectID* GetObjectID(int id);

	/**
	 * @brief Returns the activated camera, or nullptr when none is activated.
	 *
	 * Looks ActiveCameraID() up in cam_list, so a UID left over from a removed
	 * camera reads as "none" rather than as a dangling pointer.
	 *
	 * @return Non-owning pointer to the activated Camera, or nullptr.
	 * @see ActivateCamera
	 */
	Camera* GetActiveCamera();

	/**
	 * @brief Retrieves an object by its unique ID from the master pool (const).
	 * @param id Unique object identifier.
	 * @return Const pointer to ObjectID, or nullptr if not found.
	 */
	const ObjectID* GetObjectID(int id) const;

	/**
	 * @brief Returns the activated camera, or nullptr when none is activated (const).
	 * @see GetActiveCamera()
	 */
	const Camera* GetActiveCamera() const;

	// -------------------------------------------------------------------
	// Scene-wide operations
	// -------------------------------------------------------------------

	/**
	 * @brief Updates all object transforms in the scene hierarchy.
	 */
	void UpdateObjTransforms();

private:
	SceneModifStatus sc_status = SceneModifStatus::SceneChanged; ///< Current scene modification state

	/// UID of the activated camera, 0 = none. Deliberately not cleared when the
	/// camera is removed — see ActiveCameraID().
	int m_activeCamUid = 0;

	// Scene persistence needs the resource pool and is owned by SceneComponent.
	using UID::serialize;

	/**
	 * @brief Registers an object in both its type-specific pool and the master obj_list.
	 * @tparam T Object type (Camera, Mesh, Light, Sprite, DebugLine, DebugPoints).
	 * @param obj Shared pointer to the object.
	 * @param typePool Type-specific pool to register in.
	 * @note The typed and master maps refer to the same pooled object.
	 */
	template<typename T>
	void RegisterObject(Resource<T> obj, ResPool<T>& typePool)
	{
		if (!obj)
			throw std::invalid_argument("Scene membership requires a live object");
		const int id = obj.id();
		obj_list[id] = obj.shared();
		typePool[id] = std::move(obj);
	}
};

// ---------------------------------------------------------------------------
// Selection UID conversion
// ---------------------------------------------------------------------------

inline void SnapshotSelectionUids(const Selections<const ObjectID*>& selections,
                                  std::vector<int>& outSelectedUids, int& outActiveUid)
{
	const auto& list = selections.GetSelectedList();
	outSelectedUids.clear();
	outSelectedUids.reserve(list.size());
	for (const ObjectID* obj : list)
		if (obj) outSelectedUids.push_back(obj->GetObjectID());

	const ObjectID* active = selections.GetActiveObject();
	outActiveUid = active ? active->GetObjectID() : 0;
}

inline void RestoreSelectionUids(Scene& scene, const std::vector<int>& selectedUids, int activeUid)
{
	std::vector<const ObjectID*> list;
	list.reserve(selectedUids.size());
	for (int uid : selectedUids)
		if (const ObjectID* obj = scene.GetObjectID(uid))
			list.push_back(obj);

	const ObjectID* active = activeUid != 0 ? scene.GetObjectID(activeUid) : nullptr;
	scene.selections.RestoreState(std::move(list), active);
}

} // namespace neurus

