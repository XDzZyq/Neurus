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

	// The pool was restored first (ResourceComponent is registered before
	// SceneComponent); resolve the Scene's pending ID references against it.
	//
	// A scene with zero cameras is loaded as-is. Nothing is injected: the viewport
	// looks through the Editor's own camera unless the scene names an activated one
	// (Editor::ViewCamera()), so "no camera" is a legal, fully renderable document
	// rather than a state to repair behind the user's back.
	m_scene->ResolveReferences(*m_resources);
}

} // namespace neurus::project
