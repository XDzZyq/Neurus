/**
 * @file EditorComponent.cpp
 * @brief Editor viewport view-state (viewport camera UID) persistence.
 */

#include "asset/components/EditorComponent.h"

#include "core/Log.h"
#include "core/Serialize.h"
#include "editor/viewport/EditorViewport.h"

namespace neurus::project
{

EditorComponent::EditorComponent(EditorViewport& viewport)
	: m_viewport(&viewport)
{}

void EditorComponent::Save(cereal::JSONOutputArchive& ar) const
{
	const int camUid = m_viewport->CameraID();
	ar.setNextName("m_editor");
	ar.startNode();
	ar(cereal::make_nvp("viewportCamUid", camUid));
	ar.finishNode();
}

void EditorComponent::Load(cereal::JSONInputArchive& ar)
{
	// 0 means "no saved id": the viewport mints a fresh camera for it, so no
	// default pose or framing is restated here.
	int camUid = 0;

	try
	{
		ar.setNextName("m_editor");
		ar.startNode();

		// "viewportCamUid" is what this build writes; "editorCamUid" is the
		// pre-rename spelling, still read so an older project keeps its saved
		// view. Note that archive::Optional returns how many fields were
		// ABSENT, so the legacy read is gated on != 0.
		if (archive::Optional(ar, cereal::make_nvp("viewportCamUid", camUid)) != 0)
			archive::Optional(ar, cereal::make_nvp("editorCamUid", camUid));

		ar.finishNode();
	}
	catch (const cereal::Exception& e)
	{
		// Missing "m_editor" node: a project saved before the viewport camera
		// was persisted. Expected, so logged at info level.
		NEURUS_LOG("EditorComponent::Load: " << e.what() << " - using a fresh viewport camera.");
		camUid = 0;
	}

	m_viewport->RestoreCameraID(camUid);
}

} // namespace neurus::project
