#include "asset/components/SceneComponent.h"
#include "core/Log.h"
#include "core/ResourceManager.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

namespace neurus::project
{

SceneComponent::SceneComponent(Scene& scene, ResourceManager& resources)
	: m_scene(&scene)
	, m_resources(&resources)
{
}

void SceneComponent::Save(cereal::JSONOutputArchive& ar) const
{
	ar(cereal::make_nvp("m_scene", *m_scene));
}

void SceneComponent::Load(cereal::JSONInputArchive& ar)
{
	try
	{
		ar(cereal::make_nvp("m_scene", *m_scene));
	}
	catch (const cereal::Exception& e)
	{
		NEURUS_ERR("SceneComponent::Load: " << e.what() << " - using empty scene.");
	}

	// Resolve the Scene's pending ID references against the resource pool,
	// which was restored before this component by registration order.
	//
	// Nothing is injected for a camera: "no camera" is a legal, fully
	// renderable document rather than a state to repair.
	m_scene->ResolveReferences(*m_resources);
}

} // namespace neurus::project
