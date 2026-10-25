/**
 * @file Scene.h
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
#include <string>
#include <unordered_map>
#include <vector>

#include <cereal/types/memory.hpp>
#include <cereal/types/unordered_map.hpp>

#include "scene/ObjectID.h"
#include "core/Selections.h"
#include "core/Serialize.h"

#include "Camera.h"
#include "DebugLine.h"
#include "DebugMesh.h"
#include "DebugPoints.h"
#include "Light.h"
#include "Mesh.h"
#include "Sprite.h"
#include "Environment.h"

namespace neurus
{

class Scene;
class ResourceManager;

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
	using Resource = std::shared_ptr<Object>;

	/**
	 * @brief Typed object pool indexed by unique object ID.
	 *
	 * Maps an object's UID to its shared_ptr, enabling O(1) lookup and removal.
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

	/**
	 * @brief Cereal serialization for scene ID references and selection state.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note Scene owns no pooled objects: it serializes only typed ID lists
	 *       (the ResourceManager pool owns and serializes the real objects),
	 *       which ResolveReferences later restores into the pools.
	 * @note Selection is persisted as object UIDs and restored after obj_list
	 *       is rebuilt; legacy files without the block load with none.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		if constexpr (Archive::is_saving::value)
		{
			// Save each typed pool as an ID list (objects live in the pool).
			std::vector<int> camIds, meshIds, lightIds, spriteIds, dLineIds, dPointsIds, envIds;
			for (const auto& [id, obj] : cam_list)     { (void)obj; camIds.push_back(id); }
			for (const auto& [id, obj] : mesh_list)    { (void)obj; meshIds.push_back(id); }
			for (const auto& [id, obj] : light_list)   { (void)obj; lightIds.push_back(id); }
			for (const auto& [id, obj] : sprite_list)  { (void)obj; spriteIds.push_back(id); }
			for (const auto& [id, obj] : dLine_list)   { (void)obj; dLineIds.push_back(id); }
			for (const auto& [id, obj] : dPoints_list) { (void)obj; dPointsIds.push_back(id); }
			for (const auto& [id, obj] : env_list)     { (void)obj; envIds.push_back(id); }
			ar(CEREAL_NVP(camIds), CEREAL_NVP(meshIds), CEREAL_NVP(lightIds),
			   CEREAL_NVP(spriteIds), CEREAL_NVP(dLineIds), CEREAL_NVP(dPointsIds),
			   CEREAL_NVP(envIds));

			// Persist selection as UIDs (pointers are not serializable).
			std::vector<int> selectedUids;
			int activeUid = 0;
			SnapshotSelectionUids(selections, selectedUids, activeUid);
			ar(CEREAL_NVP(selectedUids), CEREAL_NVP(activeUid));

			// DebugMesh arrived after the block above; it is written last and
			// read back optionally so files predating it still load.
			std::vector<int> dMeshIds;
			for (const auto& [id, obj] : dMesh_list) { (void)obj; dMeshIds.push_back(id); }
			ar(CEREAL_NVP(dMeshIds));

			// Which scene camera the viewport looks through (0 = none is
			// legal). Written even when stale; the load path re-validates it
			// against cam_list.
			int activeCamUid = m_activeCamUid;
			ar(CEREAL_NVP(activeCamUid));
		}
		else
		{
			// Read ID lists into pending members; SceneComponent drives
			// ResolveReferences() afterwards to fill the typed pools.
			std::vector<int> camIds, meshIds, lightIds, spriteIds, dLineIds, dPointsIds, envIds;
			if (!archive::OptionalBlock(ar,
			        CEREAL_NVP(camIds), CEREAL_NVP(meshIds), CEREAL_NVP(lightIds),
			        CEREAL_NVP(spriteIds), CEREAL_NVP(dLineIds), CEREAL_NVP(dPointsIds),
			        CEREAL_NVP(envIds)))
			{
				// Legacy file (old full-pool format): degrade to an empty scene.
				camIds.clear(); meshIds.clear(); lightIds.clear();
				spriteIds.clear(); dLineIds.clear(); dPointsIds.clear(); envIds.clear();
			}
			m_pendingCamIds = std::move(camIds);
			m_pendingMeshIds = std::move(meshIds);
			m_pendingLightIds = std::move(lightIds);
			m_pendingSpriteIds = std::move(spriteIds);
			m_pendingDLineIds = std::move(dLineIds);
			m_pendingDPointsIds = std::move(dPointsIds);
			m_pendingEnvIds = std::move(envIds);

			// Restore selection UIDs (missing block = legacy file -> no selection).
			// Keys must match the save branch ("selectedUids"/"activeUid").
			std::vector<int> selectedUids;
			int activeUid = 0;
			if (!archive::OptionalBlock(ar, CEREAL_NVP(selectedUids), CEREAL_NVP(activeUid)))
			{
				selectedUids.clear();
				activeUid = 0;
			}
			m_pendingSelectedUids = std::move(selectedUids);
			m_pendingActiveUid = activeUid;

			// Optional trailing block (see the save branch): absent in files
			// written before DebugMesh existed, which load with no debug meshes.
			std::vector<int> dMeshIds;
			if (!archive::OptionalBlock(ar, CEREAL_NVP(dMeshIds)))
			{
				dMeshIds.clear();
			}
			m_pendingDMeshIds = std::move(dMeshIds);

			// Activated camera. Three states must stay distinguishable, so this
			// reads the raw OptionalBlock rather than a defaulting helper:
			//   >0  an explicit camera UID
			//    0  explicitly "no scene camera activated"
			//   -1  field absent -> file predates activation; ResolveReferences
			//       migrates it once by activating the first camera it finds.
			int activeCamUid = -1;
			if (!archive::OptionalBlock(ar, CEREAL_NVP(activeCamUid)))
			{
				activeCamUid = -1;
			}
			m_pendingActiveCamUid = activeCamUid;
		}
	}

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

	/// Selection state. Persisted as UIDs by serialize() and resolved to
	/// pointers on load.
	Selections<const ObjectID*> selections;

	/**
	 * @brief Resolves the pending ID references against the resource pool.
	 *
	 * Fills the typed pools from the ResourceManager (fetch by ID and cast),
	 * rebuilds obj_list and restores the selection. Must run after the pool
	 * has been deserialized (pool first, then Scene).
	 *
	 * @param resources The ResourceManager pool (transient parameter, never a member).
	 */
	void ResolveReferences(ResourceManager& resources);

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
		auto nameIt = m_nameToId.find(it->second->o_name);
		if (nameIt != m_nameToId.end() && nameIt->second == id)
			m_nameToId.erase(nameIt);
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
	 * @brief Looks up a scene object by its display name.
	 *
	 * Names are unique within a scene: registration keeps the requested name
	 * when it is free and otherwise appends `.001`, `.002`, ... (lowest free).
	 * @param name Display name as stored in `ObjectID::o_name`.
	 * @return Const pointer to the object, or nullptr when the name is absent.
	 */
	const ObjectID* FindObjectByName(const std::string& name) const;

	/**
	 * @brief Renames an object, keeping scene names unique.
	 * @param id Object UID.
	 * @param name Requested display name; uniquified when already taken.
	 * @return false when no object with @p id exists.
	 * @note The single supported rename path: writing `o_name` directly would
	 *       desynchronise the name index.
	 */
	bool SetObjectName(int id, const std::string& name);

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

	/// Display name -> object UID. Unique across every typed pool; maintained
	/// by RegisterObject / RemoveObject / RebuildObjList.
	std::unordered_map<std::string, int> m_nameToId;

	// -------------------------------------------------------------------
	// Pending ID references (populated by serialize(load), consumed by
	// ResolveReferences).
	// -------------------------------------------------------------------

	std::vector<int> m_pendingCamIds;        ///< Pending camera UIDs (from project file)
	std::vector<int> m_pendingMeshIds;       ///< Pending mesh UIDs
	std::vector<int> m_pendingLightIds;      ///< Pending light UIDs
	std::vector<int> m_pendingSpriteIds;     ///< Pending sprite UIDs
	std::vector<int> m_pendingDLineIds;      ///< Pending debug-line UIDs
	std::vector<int> m_pendingDPointsIds;    ///< Pending debug-point UIDs
	std::vector<int> m_pendingDMeshIds;      ///< Pending debug-mesh UIDs
	std::vector<int> m_pendingEnvIds;        ///< Pending environment UIDs
	std::vector<int> m_pendingSelectedUids;  ///< Pending selection UIDs
	int             m_pendingActiveUid = 0;  ///< Pending active-object UID
	int             m_pendingActiveCamUid = -1; ///< Pending activated-camera UID (-1 = field absent)

	/** @brief Clears all pending reference lists (legacy-file fallback). */
	void ClearPendingReferences();

	/// @brief Returns @p requested, or its lowest-free `.NNN` suffix variant.
	std::string MakeUniqueName(const std::string& requested) const;

	/// @brief Assigns the unique name to the object and records it in the index.
	void IndexObjectName(int uid, const std::string& requested);

	/**
	 * @brief Rebuilds obj_list from all typed pools after deserialization.
	 */
	void RebuildObjList();

	/**
	 * @brief Registers an object in both its type-specific pool and the master obj_list.
	 * @tparam T Object type (Camera, Mesh, Light, Sprite, DebugLine, DebugPoints).
	 * @param obj Shared pointer to the object.
	 * @param typePool Type-specific pool to register in.
	 * @note Stores an aliasing shared_ptr<ObjectID> in obj_list, sharing
	 *       ownership with the typed shared_ptr.
	 */
	template<typename T>
	void RegisterObject(Resource<T> obj, ResPool<T>& typePool)
	{
		auto* basePtr = static_cast<ObjectID*>(obj.get());
		int id = basePtr->GetObjectID();
		typePool[id] = obj;
		obj_list[id] = Resource<ObjectID>(obj, basePtr);
		IndexObjectName(id, basePtr->o_name);
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

