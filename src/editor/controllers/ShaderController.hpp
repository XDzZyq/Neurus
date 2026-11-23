/**
 * @file ShaderController.hpp
 * @brief Shader lifecycle controller - event-driven, no per-frame polling.
 *
 * Each discrete field or pooled-stage request submits its own operation. Only
 * a mesh code-edit gesture holds state across events, to record its initial
 * and final source when the editor loses focus.
 */

#pragma once

#include <string>

#include "editor/controllers/Controllers.hpp"

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