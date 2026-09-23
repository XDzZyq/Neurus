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
	m_scene->ResolveReferences(*m_resources);

	if (m_scene->cam_list.empty())
	{
		NEURUS_ERR("[SceneComponent] No camera in scene, adding default camera.");
		// Create through the pool so the fallback camera persists on the next save.
		auto defaultCam = m_resources->Load<Camera>();
		defaultCam->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
		defaultCam->cam_tar = glm::vec3(0.0f, 0.0f, 0.0f);
		m_scene->UseCamera(defaultCam);
		// Behaviour-preserving: this injected camera was the one the old
		// positional GetActiveCamera() returned, so activate it explicitly.
		m_scene->ActivateCamera(defaultCam->GetObjectID());
	}
}

} // namespace neurus::project
