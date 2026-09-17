/**
 * @file Scene.cpp
 * @brief Scene container implementation - status tracking and object lookup.
 */

#include "Scene.h"

#include "core/Log.h"
#include "core/ResourceManager.h"

namespace neurus
{

namespace
{

/**
 * @brief Fills a typed scene pool from the pending ID list by fetching + casting.
 *
 * @tparam T Scene object type (Camera, Mesh, Light, ...).
 * @param resources The ResourceManager pool.
 * @param ids Pending object UIDs read from the project file.
 * @param pool The scene's typed pool to fill.
 */
template<typename T>
void ResolvePool(ResourceManager& resources, const std::vector<int>& ids, Scene::ResPool<T>& pool)
{
	for (int id : ids)
	{
		auto resource = resources.Get<UID>(id);
		auto typed = std::dynamic_pointer_cast<T>(resource);
		if (!typed)
		{
			NEURUS_LOG("[Scene] ResolveReferences: stale/missing object UID " << id);
			continue;
		}
		pool[id] = typed;
	}
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Scene::Scene() = default;

Scene::~Scene() = default;

// ---------------------------------------------------------------------------
// Reference resolution (pool-first load ordering)
// ---------------------------------------------------------------------------

void Scene::ClearPendingReferences()
{
	m_pendingCamIds.clear();
	m_pendingMeshIds.clear();
	m_pendingLightIds.clear();
	m_pendingSpriteIds.clear();
	m_pendingDLineIds.clear();
	m_pendingDPointsIds.clear();
	m_pendingDMeshIds.clear();
	m_pendingEnvIds.clear();
	m_pendingSelectedUids.clear();
	m_pendingActiveUid = 0;
}

void Scene::ResolveReferences(ResourceManager& resources)
{
	ResolvePool<Camera>(resources, m_pendingCamIds, cam_list);
	ResolvePool<Mesh>(resources, m_pendingMeshIds, mesh_list);
	ResolvePool<Light>(resources, m_pendingLightIds, light_list);
	ResolvePool<Sprite>(resources, m_pendingSpriteIds, sprite_list);
	ResolvePool<DebugLine>(resources, m_pendingDLineIds, dLine_list);
	ResolvePool<DebugPoints>(resources, m_pendingDPointsIds, dPoints_list);
	ResolvePool<DebugMesh>(resources, m_pendingDMeshIds, dMesh_list);
	ResolvePool<Environment>(resources, m_pendingEnvIds, env_list);

	// Per-object data-resource wiring (Mesh -> MeshData/Shader, Environment ->
	// ImageData) is a POOL-wide concern handled by ResourceComponent::Load -
	// the pool's own restore step - so pooled orphans (undo history) stay
	// self-contained and uploadable after a reload.

	// Rebuild obj_list from the now-populated typed pools, then restore the
	// selection (mirrors the pre-resource-manager load behavior).
	RebuildObjList();
	RestoreSelectionUids(*this, m_pendingSelectedUids, m_pendingActiveUid);

	ClearPendingReferences();
}

// ---------------------------------------------------------------------------
// Status tracking
// ---------------------------------------------------------------------------

void Scene::UpdateSceneStatus(int tar, bool value)
{
	if (value)
	{
		sc_status = static_cast<SceneModifStatus>(static_cast<int>(sc_status) | tar);
	}
	else
	{
		sc_status = static_cast<SceneModifStatus>(static_cast<int>(sc_status) & ~tar);
	}
}

void Scene::SetSceneStatus(int tar, bool /*value*/)
{
	sc_status = static_cast<SceneModifStatus>(tar);
}

bool Scene::CheckStatus(SceneModifStatus tar)
{
	return (static_cast<int>(sc_status) & static_cast<int>(tar)) != 0;
}

void Scene::ResetStatus()
{
	sc_status = SceneModifStatus::NoChanges;
}

// ---------------------------------------------------------------------------
// Object lookup
// ---------------------------------------------------------------------------

ObjectID* Scene::GetObjectID(int id)
{
	auto it = obj_list.find(id);
	if (it != obj_list.end())
	{
		return it->second.get();
	}
	return nullptr;
}

Camera* Scene::GetActiveCamera()
{
	if (!cam_list.empty())
	{
		return cam_list.begin()->second.get();
	}
	return nullptr;
}

const ObjectID* Scene::GetObjectID(int id) const
{
	auto it = obj_list.find(id);
	if (it != obj_list.end())
	{
		return it->second.get();
	}
	return nullptr;
}

const Camera* Scene::GetActiveCamera() const
{
	if (!cam_list.empty())
	{
		return cam_list.begin()->second.get();
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Scene-wide operations
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Post-deserialization rebuild
// ---------------------------------------------------------------------------

void Scene::RebuildObjList()
{
	obj_list.clear();

	// Lambda that inserts aliasing shared_ptr<ObjectID> for each entry in a pool.
	auto rebuild = [this](auto& pool)
	{
		for (auto& [id, obj] : pool)
		{
			auto* basePtr = static_cast<ObjectID*>(obj.get());
			int id_obj = basePtr->GetObjectID();
			obj_list[id_obj] = Resource<ObjectID>(obj, basePtr);
		}
	};

	rebuild(cam_list);
	rebuild(mesh_list);
	rebuild(light_list);
	rebuild(sprite_list);
	rebuild(dLine_list);
	rebuild(dPoints_list);
	rebuild(dMesh_list);
	rebuild(env_list);
}

// ---------------------------------------------------------------------------
// Scene-wide operations
// ---------------------------------------------------------------------------

void Scene::UpdateObjTransforms()
{
	for (auto& [id, obj] : obj_list)
	{
		void* transformVoid = obj->GetTransform();
		if (transformVoid)
		{
			auto* t3d = static_cast<Transform3D*>(transformVoid);
			t3d->GetModelMatrix(); // Force cached matrix recomputation if dirty.
		}
	}
}

} // namespace neurus
