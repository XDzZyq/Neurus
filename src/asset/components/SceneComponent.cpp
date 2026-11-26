#include "asset/components/SceneComponent.hpp"

#include "core/Log.hpp"
#include "asset/ResourceManager.hpp"
#include "core/Serialize.hpp"
#include "scene/Scene.hpp"

#include <cereal/types/vector.hpp>

#include <algorithm>

namespace neurus::project
{
namespace
{

template<class T>
struct MemberList
{
	Scene& scene;
	const Scene::ResPool<T>& members;
	void (Scene::*use)(Scene::Resource<T>);

	template<class Archive>
	void save(Archive& ar) const
	{
		ar(cereal::make_size_tag(static_cast<cereal::size_type>(members.size())));
		for (const auto& [_, member] : members)
			ar(member);
	}

	template<class Archive>
	void load(Archive& ar)
	{
		cereal::size_type count = 0;
		ar(cereal::make_size_tag(count));
		ids.resize(count);
		for (auto& id : ids)
			ar(id);
	}

	void AddToScene(const IResourceLookup& pool) const
	{
		for (const int id : ids)
		{
			try
			{
				if (auto member = pool.Get<T>(id))
					(scene.*use)(std::move(member));
				else
					NEURUS_LOG("[SceneComponent] Missing scene member UID " << id);
			}
			catch (const std::exception& error)
			{
				NEURUS_ERR("[SceneComponent] Could not restore scene member UID " << id << ": " << error.what());
			}
		}
	}

	std::vector<int> ids;
};

struct SceneRecord
{
	Scene& scene;
	const IResourceLookup& pool;

	template<class T>
	auto Members(const char* key, const Scene::ResPool<T>& list, void (Scene::*use)(Scene::Resource<T>))
	{
		return cereal::make_nvp(key, MemberList<T>{scene, list, use, {}});
	}

	template<class Archive, class... Fields>
	void TransferMembers(Archive& ar, Fields&&... fields)
	{
		if constexpr (Archive::is_loading::value)
		{
			if (archive::OptionalBlock(ar, fields...))
				(fields.value.AddToScene(pool), ...);
		}
		else
			ar(fields...);
	}

	template<class Archive>
	void serialize(Archive& ar)
	{
		// These membership arrays form one group. A malformed group contributes no members.
		TransferMembers(ar,
			Members("camIds", scene.cam_list, &Scene::UseCamera),
			Members("meshIds", scene.mesh_list, &Scene::UseMesh),
			Members("lightIds", scene.light_list, &Scene::UseLight),
			Members("spriteIds", scene.sprite_list, &Scene::UseSprite),
			Members("dLineIds", scene.dLine_list, &Scene::UseDebugLine),
			Members("dPointsIds", scene.dPoints_list, &Scene::UseDebugPoints),
			Members("envIds", scene.env_list, &Scene::UseEnvironment));

		std::vector<int> selectedUids;
		int activeUid = 0;
		if constexpr (Archive::is_saving::value)
			SnapshotSelectionUids(scene.selections, selectedUids, activeUid);
		const bool hasSelection = archive::OptionalBlock(ar, CEREAL_NVP(selectedUids), CEREAL_NVP(activeUid));

		TransferMembers(ar, Members("dMeshIds", scene.dMesh_list, &Scene::UseDebugMesh));
		int activeCamUid = scene.ActiveCameraID();
		archive::OptionalField(ar, CEREAL_NVP(activeCamUid));
		if constexpr (Archive::is_loading::value)
		{
			if (hasSelection)
				RestoreSelectionUids(scene, selectedUids, activeUid);
			scene.RestoreActiveCameraID(std::max(0, activeCamUid));
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
