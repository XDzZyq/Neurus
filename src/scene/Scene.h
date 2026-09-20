/**
 * @file Scene.h
 * @brief Scene container holding all scene objects and scene-wide state.
 *
 * Provides the Scene class which aggregates cameras, lights, meshes,
 * sprites, debug primitives, and other renderable objects into a unified
 * scene graph via typed object pools (ResPool).
 *
 * Architecture:
 * - Scene: Owns all scene objects via shared_ptr maps, indexed by UID
 * - Each object type has a dedicated ResPool (cam_list, mesh_list, etc.)
 * - A master obj_list holds all ObjectID instances for global lookup
 * - Renderer reads Scene as immutable scene graph via GetObjectID()
 *
 * Scene Modification Tracking:
 * - SceneModifStatus bitfield flags track which aspects changed
 * - Renderer can optimize updates by checking status flags
 * - Flags reset after processing to avoid redundant work
 *
 * Object Lifetime:
 * - Objects owned via shared_ptr (reference counted)
 * - Removing from map triggers destruction if no other references exist
 * - GPU resources released in object destructors
 *
 * @note Inheritance: Scene inherits UID for unique scene identifier.
 * @note Thread-safety: Not thread-safe. Access from main thread only.
 */

#pragma once

#include <memory>
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
 * @note Scene-layer conversion shared by Scene::serialize and the editor's
 *       selection snapshot; keeps pointer↔UID logic in the layer that owns it.
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
 * Scene aggregates all renderable and non-renderable objects in a scene.
 * It provides typed pools (ResPool) for each object category, enabling
 * efficient iteration and lookup by ID. Active objects (camera, environment)
 * are cached for fast access during rendering.
 *
 * Scene Modification Tracking:
 * - SceneModifStatus flags track which aspects of the scene changed
 * - Renderer can optimize updates by checking status flags
 * - Flags reset after processing to avoid redundant work
 *
 * Object Lifetime:
 * - Objects owned via shared_ptr (reference counted)
 * - Removing from map triggers destruction if no other references exist
 * - GPU resources released in object destructors
 *
 * @note Inheritance: UID provides unique scene identifier.
 * @note Thread-safety: Not thread-safe. Access from main thread only.
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
	 * Maps an object's UID (int) to its shared_ptr. Enables O(1) lookup
	 * and removal of objects from the scene.
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
	 * Bitfield enum that tracks which aspects of the scene have been
	 * modified since the last render cycle. Renderer queries these
	 * flags to determine which resources need updating.
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

	/**
	 * @brief Constructs an empty scene.
	 */
	Scene();

	/**
	 * @brief Destroys the scene and all owned objects.
	 * @note GPU resources released via object destructors.
	 */
	~Scene();

	/**
	 * @brief Cereal serialization for scene ID references and selection state.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note Scene owns no pooled objects: it serializes ONLY typed ID lists
	 *       (the ResourceManager pool owns and serializes the real objects).
	 *       obj_list (master pool) is reconstructed from the typed pools by
	 *       ResolveReferences after the ID lists resolve.
	 * @note Selection is stored at runtime as pointers (const ObjectID*) but
	 *       persisted as object UIDs; ResolveReferences restores the selection
	 *       AFTER obj_list is rebuilt. The selection block is optional:
	 *       legacy project files that predate it load with an empty selection.
	 * @note Legacy files (old full-pool "m_scene" format) fail to read the ID
	 *       lists and degrade to an empty scene + default-camera fallback
	 *       (SceneComponent).
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
		}
	}

	// -------------------------------------------------------------------
	// Status tracking
	// -------------------------------------------------------------------

	/**
	 * @brief Updates scene status flag (bitwise OR on set, AND NOT on clear).
	 * @param tar Target status flag(s) to modify (int for OR-of-enum convenience).
	 * @param value If true, set flag(s); if false, clear flag(s).
	 * @note Multiple flags can be combined via bitwise OR in tar.
	 */
	void UpdateSceneStatus(int tar, bool value);

	/**
	 * @brief Directly sets scene status (replaces existing value).
	 * @param tar New status bitfield value.
	 * @param value Unused (legacy parameter, kept for API compatibility).
	 */
	void SetSceneStatus(int tar, bool value);

	/**
	 * @brief Checks if a specific status flag is set.
	 * @param tar Status flag(s) to check.
	 * @return True if any of the specified flags are set.
	 */
	bool CheckStatus(SceneModifStatus tar);

	/**
	 * @brief Resets scene status to NoChanges.
	 * @note Called after Renderer processes all scene updates.
	 */
	void ResetStatus();

	// -------------------------------------------------------------------
	// Object pools (public for direct iteration by Renderer)
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

	/// Selection state for scene objects. Persisted as UIDs by serialize()
	/// (see the serialize note); resolved to pointers on load.
	/// Stores const ObjectID* — compatible with both Editor (mutable objects via GetObjectID)
	/// and Outliner (const ObjectID* from GetObjectIDs enumeration).
	Selections<const ObjectID*> selections;

	/**
	 * @brief Resolves the pending ID references against the resource pool.
	 *
	 * Fills the typed pools from the ResourceManager (fetched by ID + cast),
	 * rebuilds obj_list, and restores the selection. Called by
	 * SceneComponent::Load AFTER the pool has been deserialized (pool first,
	 * then Scene). Per-object data-resource wiring (Mesh -> MeshData/Shader,
	 * Environment -> ImageData) is NOT here - it is a pool-wide concern done
	 * by ResourceComponent::Load (the pool's own restore step), so pooled
	 * orphans (undo history) stay self-contained and uploadable.
	 *
	 * @param resources The ResourceManager pool (transient parameter, never a member).
	 */
	void ResolveReferences(ResourceManager& resources);

	// -------------------------------------------------------------------
	// Registration - store in both type-specific pool AND obj_list
	// -------------------------------------------------------------------

	/**
	 * @brief Registers a camera in the scene.
	 * @param camera Shared pointer to Camera object.
	 * @note Adds to cam_list and obj_list.
	 */
	void UseCamera(Resource<Camera> camera)
	{
		RegisterObject(camera, cam_list);
	}

	/**
	 * @brief Registers a mesh in the scene.
	 * @param mesh Shared pointer to Mesh object.
	 * @note Adds to mesh_list and obj_list.
	 */
	void UseMesh(Resource<Mesh> mesh)
	{
		RegisterObject(mesh, mesh_list);
	}

	/**
	 * @brief Registers a light in the scene.
	 * @param light Shared pointer to Light object.
	 * @note Adds to light_list and obj_list.
	 */
	void UseLight(Resource<Light> light)
	{
		RegisterObject(light, light_list);
	}

	/**
	 * @brief Registers a sprite in the scene.
	 * @param sprite Shared pointer to Sprite object.
	 * @note Adds to sprite_list and obj_list.
	 */
	void UseSprite(Resource<Sprite> sprite)
	{
		RegisterObject(sprite, sprite_list);
	}

	/**
	 * @brief Registers debug lines in the scene.
	 * @param dline Shared pointer to DebugLine object.
	 * @note Adds to dLine_list and obj_list.
	 */
	void UseDebugLine(Resource<DebugLine> dline)
	{
		RegisterObject(dline, dLine_list);
	}

	/**
	 * @brief Registers debug points in the scene.
	 * @param dpoints Shared pointer to DebugPoints object.
	 * @note Adds to dPoints_list and obj_list.
	 */
	void UseDebugPoints(Resource<DebugPoints> dpoints)
	{
		RegisterObject(dpoints, dPoints_list);
	}

	/**
	 * @brief Registers a debug wireframe mesh in the scene.
	 * @param dmesh Shared pointer to DebugMesh object.
	 * @note Adds to dMesh_list and obj_list.
	 */
	void UseDebugMesh(Resource<DebugMesh> dmesh)
	{
		RegisterObject(dmesh, dMesh_list);
	}

	/**
	 * @brief Registers an environment object in the scene.
	 * @param env Shared pointer to Environment object.
	 * @note Adds to env_list and obj_list.
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
	 * @note Mirrors RegisterObject(): erases from the typed pool and the
	 *       master obj_list and clears the object from the selection. The
	 *       pooled resource itself is NOT removed from the ResourceManager —
	 *       deleting only drops the scene reference, so the operation stays
	 *       invertible (undo re-registers without any reload).
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
	 * @brief Returns the active camera (first in cam_list).
	 * @return Non-owning pointer to Camera, or nullptr if no cameras.
	 * @note First camera in cam_list is considered active.
	 */
	Camera* GetActiveCamera();

	/**
	 * @brief Retrieves an object by its unique ID from the master pool (const).
	 * @param id Unique object identifier.
	 * @return Const pointer to ObjectID, or nullptr if not found.
	 */
	const ObjectID* GetObjectID(int id) const;

	/**
	 * @brief Returns the active camera (first in cam_list) (const).
	 * @return Const pointer to Camera, or nullptr if no cameras.
	 */
	const Camera* GetActiveCamera() const;

	// -------------------------------------------------------------------
	// Scene-wide operations
	// -------------------------------------------------------------------

	/**
	 * @brief Updates all object transforms in the scene hierarchy.
	 * @note Stub for MVP - implementation deferred until scene objects exist.
	 */
	void UpdateObjTransforms();

private:
	SceneModifStatus sc_status = SceneModifStatus::SceneChanged; ///< Current scene modification state

	// -------------------------------------------------------------------
	// Pending ID references (populated by serialize(load), consumed by
	// ResolveReferences). The pool is a transient parameter, never a member.
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

	/** @brief Clears all pending reference lists (legacy-file fallback). */
	void ClearPendingReferences();

	/**
	 * @brief Rebuilds obj_list from all typed pools after deserialization.
	 *
	 * Iterates each typed pool (cam_list, mesh_list, etc.) and inserts
	 * aliasing shared_ptr<ObjectID> entries into obj_list, matching the
	 * same pattern as RegisterObject().
	 */
	void RebuildObjList();

	/**
	 * @brief Registers an object in both its type-specific pool and the master obj_list.
	 * @tparam T Object type (Camera, Mesh, Light, Sprite, DebugLine, DebugPoints).
	 * @param obj Shared pointer to the object.
	 * @param typePool Type-specific pool to register in.
	 * @note Uses shared_ptr aliasing constructor to store the base ObjectID* in
	 *       obj_list while sharing ownership with the typed shared_ptr.
	 */
	template<typename T>
	void RegisterObject(Resource<T> obj, ResPool<T>& typePool)
	{
		auto* basePtr = static_cast<ObjectID*>(obj.get());
		int id = basePtr->GetObjectID();
		typePool[id] = obj;
		obj_list[id] = Resource<ObjectID>(obj, basePtr);
	}
};

// ---------------------------------------------------------------------------
// Selection UID conversion (shared by serialize and the editor controller)
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

