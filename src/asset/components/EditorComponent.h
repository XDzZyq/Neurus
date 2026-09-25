#pragma once

#include "asset/Serializable.h"

namespace neurus {
class Editor;
}

namespace neurus::project {

/**
 * @brief Serialization adapter for Editor-owned view state.
 *
 * Carries exactly one field today: the pool UID of the **editor camera** — the
 * camera the viewport looks through unless a scene camera is activated. The
 * camera *object* needs no adapter, because it is a pooled resource and
 * ResourceComponent serializes the ResourceManager pool wholesale; only the
 * Editor's claim on which pooled camera is its own has to be recorded.
 *
 * Registered LAST in Project (after HistoryComponent): registration order is
 * archive read order, so appending puts the new node where a file written before
 * it existed simply ends, and Load() below treats that as "no saved id".
 *
 * Like HistoryComponent, this is a reverse dependency from asset to editor, kept
 * in the .cpp only — every final binary links editor alongside asset.
 */
class EditorComponent : public Serializable
{
public:
	explicit EditorComponent(Editor& editor);

	const char* Key() const noexcept override { return "m_editor"; }

	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;

private:
	Editor* m_editor;
};

} // namespace neurus::project
