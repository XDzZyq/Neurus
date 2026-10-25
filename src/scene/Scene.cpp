/**
 * @file Scene.cpp
 * @brief Scene container implementation - status tracking and object lookup.
 */

#include "Scene.h"

#include <algorithm>
#include <cstdio>

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
	m_pendingActiveCamUid = -1;
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

	// Per-object data-resource wiring is the pool's own restore step and is
	// deliberately not done here, so pooled orphans (undo history) stay
	// self-contained and uploadable after a reload.

	// Rebuild obj_list from the now-populated typed pools, then restore the
	// selection (mirrors the pre-resource-manager load behavior).
	RebuildObjList();
	RestoreSelectionUids(*this, m_pendingSelectedUids, m_pendingActiveUid);

	// Camera activation. cam_list is populated by now, so the UID can be
	// validated; -1 means the file predates the field: migrate once by
	// activating whichever camera the old positional GetActiveCamera() would
	// have picked. The next save writes the field explicitly.
	if (m_pendingActiveCamUid >= 0)
	{
		m_activeCamUid = m_pendingActiveCamUid;
	}
	else
	{
		m_activeCamUid = cam_list.empty() ? 0 : cam_list.begin()->first;
	}

	ClearPendingReferences();
}

bool Scene::ActivateCamera(int uid)
{
	if (cam_list.find(uid) == cam_list.end())
	{
		NEURUS_ERR("[Scene] ActivateCamera: UID " << uid << " is not a scene camera");
		return false;
	}
	m_activeCamUid = uid;
	return true;
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
	auto it = cam_list.find(m_activeCamUid);
	return (it != cam_list.end()) ? it->second.get() : nullptr;
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
	auto it = cam_list.find(m_activeCamUid);
	return (it != cam_list.end()) ? it->second.get() : nullptr;
}

// ---------------------------------------------------------------------------
// Name index
// ---------------------------------------------------------------------------

const ObjectID* Scene::FindObjectByName(const std::string& name) const
{
	auto it = m_nameToId.find(name);
	if (it == m_nameToId.end())
	{
		return nullptr;
	}
	return GetObjectID(it->second);
}

bool Scene::SetObjectName(int id, const std::string& name)
{
	ObjectID* obj = GetObjectID(id);
	if (obj == nullptr)
	{
		return false;
	}

	// Free the current name first, so renaming an object to its own name is a
	// no-op instead of suffixing it.
	auto old = m_nameToId.find(obj->o_name);
	if (old != m_nameToId.end() && old->second == id)
	{
		m_nameToId.erase(old);
	}

	const std::string unique = MakeUniqueName(name);
	obj->o_name = unique;
	m_nameToId[unique] = id;
	return true;
}

std::string Scene::MakeUniqueName(const std::string& requested) const
{
	const std::string base = requested.empty() ? std::string("Object") : requested;
	if (m_nameToId.find(base) == m_nameToId.end())
	{
		return base;
	}

	for (int suffix = 1;; ++suffix)
	{
		char buffer[16];
		std::snprintf(buffer, sizeof(buffer), ".%03d", suffix);
		std::string candidate = base + buffer;
		if (m_nameToId.find(candidate) == m_nameToId.end())
		{
			return candidate;
		}
	}
}

void Scene::IndexObjectName(int id, const std::string& requested)
{
	ObjectID* obj = GetObjectID(id);
	if (obj == nullptr)
	{
		return;
	}

	// Re-registering an object must keep the name it already owns.
	auto existing = m_nameToId.find(requested);
	if (!requested.empty() && existing != m_nameToId.end() && existing->second == id)
	{
		obj->o_name = requested;
		return;
	}

	const std::string unique = MakeUniqueName(requested);
	obj->o_name = unique;
	m_nameToId[unique] = id;
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

	// Names are collected alongside the master list so they can be re-indexed
	// deterministically: a file may hold duplicates, and unordered pool order
	// would otherwise decide which object keeps the bare name.
	std::vector<std::pair<int, std::string>> named;

	auto rebuild = [this, &named](auto& pool)
	{
		for (auto& [id, obj] : pool)
		{
			auto* basePtr = static_cast<ObjectID*>(obj.get());
			int id_obj = basePtr->GetObjectID();
			obj_list[id_obj] = Resource<ObjectID>(obj, basePtr);
			named.emplace_back(id_obj, basePtr->o_name);
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

	std::sort(named.begin(), named.end(),
	          [](const auto& a, const auto& b) { return a.first < b.first; });

	m_nameToId.clear();
	for (const auto& [id, name] : named)
	{
		IndexObjectName(id, name);
	}
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
