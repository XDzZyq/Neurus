#pragma once

#include "asset/Serializable.h"

namespace neurus {
class EditorViewport;
}

namespace neurus::project {

/**
 * @brief Serialization adapter for the editor viewport's view state.
 *
 * Carries one field: the pool UID of the viewport camera. The camera object
 * needs no adapter - it is a pooled resource, so only the viewport's claim on
 * which pooled camera is its own has to be recorded.
 *
 * Registered last in the project: registration order is archive read order, so
 * a file written before this node existed simply ends, and Load() treats that
 * as "no saved id".
 */
class EditorComponent : public Serializable
{
public:
	explicit EditorComponent(EditorViewport& viewport);

	const char* Key() const noexcept override { return "m_editor"; }

	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;

private:
	EditorViewport* m_viewport;
};

} // namespace neurus::project
