/**
 * @file EditorComponent.cpp
 * @brief Editor viewport view-state (viewport camera UID) persistence.
 */

#include "editor/components/EditorComponent.hpp"

#include "core/Log.hpp"
#include "core/Serialize.hpp"
#include "editor/viewport/EditorViewport.hpp"

namespace neurus::project
{

EditorComponent::EditorComponent(EditorViewport& viewport)
	: m_viewport(&viewport)
{}

void EditorComponent::Save(cereal::JSONOutputArchive& ar) const
{
	const auto& camera = m_viewport->CameraReference();
	ar.setNextName("m_editor");
	ar.startNode();
	archive::RefId(ar, "viewportCamUid", camera);
	ar.finishNode();
}

void EditorComponent::Load(cereal::JSONInputArchive& ar)
{
	// 0 means "no saved id": the viewport mints a fresh camera for it, so no
	// default pose or framing is restated here.
	ResourceRef<Camera> camera;

	try
	{
		ar.setNextName("m_editor");
		ar.startNode();

		archive::RefId(ar, "viewportCamUid", camera);

		ar.finishNode();
	}
	catch ([[maybe_unused]] const cereal::Exception& e)
	{
		// Missing "m_editor" node: a project saved before the viewport camera
		// was persisted. Expected, so logged at info level.
		NEURUS_LOG("EditorComponent::Load: " << e.what() << " - using a fresh viewport camera.");
		camera.reset();
	}

	m_viewport->RestoreCamera(std::move(camera));
}

} // namespace neurus::project
