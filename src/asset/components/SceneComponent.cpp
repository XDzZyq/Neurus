#include "asset/components/SceneComponent.hpp"

#include "core/Log.hpp"
#include "core/ResourceManager.hpp"
#include "core/Serialize.hpp"
#include "scene/Scene.hpp"

#include <cereal/types/vector.hpp>

namespace neurus::project
{
namespace
{

struct SceneRecord
{
	std::vector<int> camIds, meshIds, lightIds, spriteIds, dLineIds, dPointsIds, envIds;
	std::vector<int> selectedUids;
	int activeUid = 0;
	std::vector<int> dMeshIds;
	int activeCamUid = -1; // Field absent in files written before camera activation.

	template<class Archive>
	void serialize(Archive& ar)
	{
		if (!archive::OptionalBlock(ar, CEREAL_NVP(camIds), CEREAL_NVP(meshIds), CEREAL_NVP(lightIds),
		        CEREAL_NVP(spriteIds), CEREAL_NVP(dLineIds), CEREAL_NVP(dPointsIds), CEREAL_NVP(envIds)))
		{
			// The legacy full-object scene has no usable membership lists.
			*this = SceneRecord{};
		}
		if (!archive::OptionalBlock(ar, CEREAL_NVP(selectedUids), CEREAL_NVP(activeUid)))
		{
			selectedUids.clear();
			activeUid = 0;
		}
		if (!archive::OptionalBlock(ar, CEREAL_NVP(dMeshIds)))
			dMeshIds.clear();
		archive::Optional(ar, CEREAL_NVP(activeCamUid));
	}
};

template<class T>
std::vector<int> memberIds(const Scene::ResPool<T>& members)
{
	std::vector<int> ids;
	ids.reserve(members.size());
	for (const auto& entry : members)
		ids.push_back(entry.first);
	return ids;
}

template<class T>
void restoreMembers(Scene& scene, const IResourceLookup& resources, const std::vector<int>& ids,
                    void (Scene::*use)(std::shared_ptr<T>))
{
	for (const int id : ids)
	{
		try
		{
			auto member = resources.Get<T>(id);
			if (member)
				(scene.*use)(std::move(member));
			else
				NEURUS_LOG("[SceneComponent] Missing scene member UID " << id);
		}
		catch (const std::exception& e)
		{
			NEURUS_ERR("[SceneComponent] Failed to restore scene member " << id << ": " << e.what());
		}
	}
}

} // namespace

SceneComponent::SceneComponent(Scene& scene, ResourceManager& resources)
	: m_scene(&scene)
	, m_resources(&resources)
{
}

void SceneComponent::Save(cereal::JSONOutputArchive& ar) const
{
	SceneRecord record;
	record.camIds = memberIds(m_scene->cam_list);
	record.meshIds = memberIds(m_scene->mesh_list);
	record.lightIds = memberIds(m_scene->light_list);
	record.spriteIds = memberIds(m_scene->sprite_list);
	record.dLineIds = memberIds(m_scene->dLine_list);
	record.dPointsIds = memberIds(m_scene->dPoints_list);
	record.envIds = memberIds(m_scene->env_list);
	record.dMeshIds = memberIds(m_scene->dMesh_list);
	SnapshotSelectionUids(m_scene->selections, record.selectedUids, record.activeUid);
	record.activeCamUid = m_scene->ActiveCameraID();
	ar(cereal::make_nvp("m_scene", record));
}

void SceneComponent::Load(cereal::JSONInputArchive& ar)
{
	SceneRecord record;
	m_scene->ClearObjects();
	try
	{
		ar(cereal::make_nvp("m_scene", record));
	}
	catch (const cereal::Exception& e)
	{
		NEURUS_ERR("SceneComponent::Load: " << e.what() << " - using empty scene.");
		return;
	}

	restoreMembers(*m_scene, *m_resources, record.camIds, &Scene::UseCamera);
	restoreMembers(*m_scene, *m_resources, record.meshIds, &Scene::UseMesh);
	restoreMembers(*m_scene, *m_resources, record.lightIds, &Scene::UseLight);
	restoreMembers(*m_scene, *m_resources, record.spriteIds, &Scene::UseSprite);
	restoreMembers(*m_scene, *m_resources, record.dLineIds, &Scene::UseDebugLine);
	restoreMembers(*m_scene, *m_resources, record.dPointsIds, &Scene::UseDebugPoints);
	restoreMembers(*m_scene, *m_resources, record.dMeshIds, &Scene::UseDebugMesh);
	restoreMembers(*m_scene, *m_resources, record.envIds, &Scene::UseEnvironment);
	RestoreSelectionUids(*m_scene, record.selectedUids, record.activeUid);
	if (record.activeCamUid < 0)
		record.activeCamUid = m_scene->cam_list.empty() ? 0 : m_scene->cam_list.begin()->first;
	m_scene->RestoreActiveCameraID(record.activeCamUid);
}

} // namespace neurus::project
