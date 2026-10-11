/**
 * @file ShaderController.h
 * @brief Shader lifecycle controller - event-driven, no per-frame polling.
 *
 * Translates shader editor events (create, compile, code edit, struct edit) into
 * shader data mutations. Stateless: all handler logic lives as free functions in
 * the .cpp. The target is resolved by integer UID from the scene on every event,
 * so the controller never holds a raw pointer.
 *
 * Undo/redo covers content edits only: a code-editor burst is bracketed by
 * ShaderEditBegin/End and records one op on focus-out, discrete struct edits
 * record one op each, and replay uses dedicated restore events that re-apply one
 * edit dimension and bump the version without recompiling. Compile stays a
 * non-undoable lifecycle action; create is recorded by the Editor.
 */

#pragma once

#include <string>

#include "editor/controllers/Controllers.h"
#include "editor/events/EventBus.h"
#include "editor/events/ShaderEvents.h"

namespace neurus {

class ShaderController : public Controllers
{
public:
	ShaderController() = default;

	/**
	 * @brief Subscribes to shader events on the given context.
	 *
	 * Registers lambda handlers that forward each event to the corresponding
	 * free-function handler.
	 *
	 * @param ctx Controller context (events, resources, ops, scene).
	 */
	void Init(ControllerContext& ctx) override;

private:
	// --- Code-edit gesture state (ShaderEditBegin -> ShaderEditEnd) ---
	bool m_codeEditing = false;    ///< True between begin and end.
	int  m_editObjectId = 0;       ///< Object UID captured at gesture start.
	int  m_editStage = 0;          ///< Stage captured at gesture start.
	std::string m_beforeCode;      ///< Code snapshot at gesture start.
};

} // namespace neurus