/**
 * @file EditorComponent.cpp
 * @brief Editor viewport view-state (viewport camera UID) persistence.
 */

#include "editor/components/EditorComponent.hpp"

#include "core/IResourceLookup.hpp"
#include "core/Log.hpp"
#include "core/Serialize.hpp"
#include "editor/viewport/EditorViewport.hpp"

namespace neurus::project
{

EditorComponent::EditorComponent(EditorViewport& viewport, const IResourceLookup& resources)
	: m_viewport(&viewport), m_resources(resources)
{}

void EditorComponent::Save(cereal::JSONOutputArchive& ar) const
{
	int uid = m_viewport->CameraID();
	ar.setNextName("m_editor");
	ar.startNode();
	archive::OptionalField(ar, cereal::make_nvp("viewportCamUid", uid));
	ar.finishNode();
}

void EditorComponent::Load(cereal::JSONInputArchive& ar)
{
	// 0 means "no saved id": the viewport mints a fresh camera for it, so no
	// default pose or framing is restated here.
	int uid = 0;

	try
	{
		ar.setNextName("m_editor");
		ar.startNode();

		archive::OptionalField(ar, cereal::make_nvp("viewportCamUid", uid));

		ar.finishNode();
	}
	catch ([[maybe_unused]] const cereal::Exception& e)
	{
		// Missing "m_editor" node: a project saved before the viewport camera
		// was persisted. Expected, so logged at info level.
		NEURUS_LOG("EditorComponent::Load: " << e.what() << " - using a fresh viewport camera.");
		uid = 0;
	}

	m_viewport->RestoreCamera(uid > 0 ? m_resources.Get<Camera>(uid) : nullptr);
}

} // namespace neurus::project
