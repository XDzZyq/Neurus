/**
 * @file Scene.cpp
 * @brief Scene container implementation - status tracking and object lookup.
 */

#include "Scene.hpp"

#include "core/Log.hpp"

namespace neurus
{

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Scene::Scene() = default;

Scene::~Scene() = default;

void Scene::ClearObjects()
{
	selections.ClearSelection();
	DeactivateCamera();
	obj_list.clear();
	cam_list.clear();
	mesh_list.clear();
	light_list.clear();
	sprite_list.clear();
	dLine_list.clear();
	dPoints_list.clear();
	dMesh_list.clear();
	env_list.clear();
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
