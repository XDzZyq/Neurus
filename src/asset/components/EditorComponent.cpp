/**
 * @file EditorComponent.cpp
 * @brief Editor view-state (editor camera UID) persistence.
 */

#include "asset/components/EditorComponent.h"

#include "core/Log.h"
#include "editor/Editor.h"

namespace neurus::project
{

EditorComponent::EditorComponent(Editor& editor)
	: m_editor(&editor)
{}

void EditorComponent::Save(cereal::JSONOutputArchive& ar) const
{
	const int camUid = m_editor->EditorCameraID();
	ar.setNextName("m_editor");
	ar.startNode();
	ar(cereal::make_nvp("editorCamUid", camUid));
	ar.finishNode();
}

void EditorComponent::Load(cereal::JSONInputArchive& ar)
{
	try
	{
		int camUid = 0;
		ar.setNextName("m_editor");
		ar.startNode();
		ar(cereal::make_nvp("editorCamUid", camUid));
		ar.finishNode();
		m_editor->RestoreEditorCameraID(camUid);
	}
	catch (const cereal::Exception& e)
	{
		// Missing "m_editor" node: a project saved before the editor camera was
		// persisted. Expected, so logged at info level. 0 makes
		// Editor::EnsureEditorCamera() mint a fresh camera at the default framing.
		NEURUS_LOG("EditorComponent::Load: " << e.what() << " - using a fresh editor camera.");
		m_editor->RestoreEditorCameraID(0);
	}
}

} // namespace neurus::project
