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
	// 0 is the "no saved id" value, and the only one spelled here: the next
	// EditorViewport::EnsureCamera() reads it as "mint a fresh camera", so there is
	// no default pose or framing restated in this load path.
	int camUid = 0;

	try
	{
		ar.setNextName("m_editor");
		ar.startNode();

		// A rename, not a default, so it is an explicit two-step rather than an
		// Optional() over both: "viewportCamUid" is what this build writes, and
		// "editorCamUid" is what every file written before the camera moved from the
		// Editor into the EditorViewport carries. Either name resolves to the same
		// pooled camera, so an older project keeps its saved view instead of
		// silently reopening at the default framing.
		//
		// Note the comparison: archive::Optional returns how many fields were
		// ABSENT, not whether it succeeded, so the legacy read is gated on != 0.
		if (archive::Optional(ar, cereal::make_nvp("viewportCamUid", camUid)) != 0)
			archive::Optional(ar, cereal::make_nvp("editorCamUid", camUid));

		ar.finishNode();
	}
	catch (const cereal::Exception& e)
	{
		// Missing "m_editor" node entirely: a project saved before the viewport
		// camera was persisted at all. Expected, so logged at info level.
		NEURUS_LOG("EditorComponent::Load: " << e.what() << " - using a fresh viewport camera.");
		camUid = 0;
	}

	m_viewport->RestoreCameraID(camUid);
}

} // namespace neurus::project
