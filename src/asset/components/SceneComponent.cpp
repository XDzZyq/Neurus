#include "asset/components/SceneComponent.hpp"

#include "core/Log.hpp"
#include "asset/ResourceManager.hpp"
#include "core/Serialize.hpp"
#include "scene/Scene.hpp"

#include <cereal/types/vector.hpp>

namespace neurus::project
{
namespace
{

template<class T>
struct MemberList
{
	const char* key;
	const Scene::ResPool<T>& members;
	void (Scene::*use)(Scene::Resource<T>);
	std::vector<int> ids{};

	void Collect()
	{
		ids.reserve(members.size());
		for (const auto& [_, member] : members)
			ids.push_back(member.id());
	}

	void Restore(Scene& scene, const IResourceLookup& pool)
	{
		for (const int id : ids)
		{
			try
			{
				// Zero is a valid scene member UID, including in legacy projects.
				auto ref = ResourceRef<T>::FromId(id);
				ref.Resolve(pool);
				if (ref)
					(scene.*use)(std::move(ref));
				else
					NEURUS_LOG("[SceneComponent] Missing scene member UID " << id);
			}
			catch (const std::exception& e)
			{
				NEURUS_ERR("[SceneComponent] Failed to restore scene member " << id << ": " << e.what());
			}
		}
	}
};

struct SceneRecord
{
	Scene& scene;
	const IResourceLookup& pool;

	template<class Archive, class... Lists>
	void TransferMembers(Archive& ar, Lists... lists)
	{
		if constexpr (Archive::is_saving::value)
			(lists.Collect(), ...);
		if (archive::OptionalBlock(ar, cereal::make_nvp(lists.key, lists.ids)...))
			if constexpr (Archive::is_loading::value)
				(lists.Restore(scene, pool), ...);
	}

	template<class Archive>
	void serialize(Archive& ar)
	{
		// The original membership fields are one optional group. The debug mesh
		// field was added later and can be absent independently.
		TransferMembers(ar,
			MemberList{"camIds", scene.cam_list, &Scene::UseCamera},
			MemberList{"meshIds", scene.mesh_list, &Scene::UseMesh},
			MemberList{"lightIds", scene.light_list, &Scene::UseLight},
			MemberList{"spriteIds", scene.sprite_list, &Scene::UseSprite},
			MemberList{"dLineIds", scene.dLine_list, &Scene::UseDebugLine},
			MemberList{"dPointsIds", scene.dPoints_list, &Scene::UseDebugPoints},
			MemberList{"envIds", scene.env_list, &Scene::UseEnvironment});

		std::vector<int> selectedUids;
		int activeUid = 0;
		if constexpr (Archive::is_saving::value)
			SnapshotSelectionUids(scene.selections, selectedUids, activeUid);
		const bool hasSelection = archive::OptionalBlock(ar, CEREAL_NVP(selectedUids), CEREAL_NVP(activeUid));

		TransferMembers(ar, MemberList{"dMeshIds", scene.dMesh_list, &Scene::UseDebugMesh});
		int activeCamUid = scene.ActiveCameraID();
		const bool hasCamera = archive::OptionalField(ar, CEREAL_NVP(activeCamUid));
		if constexpr (Archive::is_loading::value)
		{
			if (hasSelection)
				RestoreSelectionUids(scene, selectedUids, activeUid);
			if (!hasCamera || activeCamUid < 0)
				activeCamUid = scene.cam_list.empty() ? 0 : scene.cam_list.begin()->first;
			scene.RestoreActiveCameraID(activeCamUid);
		}
	}
};

} // namespace

SceneComponent::SceneComponent(Scene& scene, ResourceManager& resources)
	: m_scene(&scene)
	, m_resources(&resources)
{
}

void SceneComponent::Save(cereal::JSONOutputArchive& ar) const
{
	SceneRecord record{*m_scene, *m_resources};
	ar(cereal::make_nvp("m_scene", record));
}

void SceneComponent::Load(cereal::JSONInputArchive& ar)
{
	m_scene->ClearObjects();
	SceneRecord record{*m_scene, *m_resources};
	try
	{
		ar(cereal::make_nvp("m_scene", record));
	}
	catch (const cereal::Exception& e)
	{
		m_scene->ClearObjects();
		NEURUS_ERR("SceneComponent::Load: " << e.what() << " - using empty scene.");
	}
}

} // namespace neurus::project
