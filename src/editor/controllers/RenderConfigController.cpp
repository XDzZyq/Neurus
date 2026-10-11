/**
 * @file RenderConfigController.cpp
 * @brief Applies RenderConfig edits and records them as undoable operations.
 *
 * The apply path is shared by live edits and operation replay: write the incoming
 * config and request a render reset. Recording is gesture-aware (see the header).
 */

#include "editor/controllers/RenderConfigController.h"

#include <memory>

#include "editor/events/ConfigEvents.h"
#include "editor/events/EditorEvents.h"
#include "editor/operations/ConfigOperations.h"

namespace neurus {

void RenderConfigController::Init(ControllerContext& ctx)
{
	ctx.events.subscribe<ConfigEditBegin>([this, ctx](const ConfigEditBegin&) {
		if (RenderConfig* cfg = ctx.config())
		{
			m_before = *cfg;
			m_editing = true;
		}
	});

	ctx.events.subscribe<RenderConfigChangedEvent>([this, ctx](const RenderConfigChangedEvent& e) {
		RenderConfig* cfg = ctx.config();
		if (!cfg) return;

		const RenderConfig before = *cfg;
		*cfg = e.config;
		ctx.events.enqueue(RenderResetEvent{});

		// During a slider gesture the drag is recorded once on ConfigEditEnd;
		// discrete edits (no gesture) record immediately. Skip no-op writes.
		if (!m_editing && !(before == e.config))
		{
			ctx.ops.Submit(std::make_unique<SetRenderConfigOp>(before, e.config));
		}
	});

	ctx.events.subscribe<ConfigEditEnd>([this, ctx](const ConfigEditEnd&) {
		if (m_editing)
		{
			if (RenderConfig* cfg = ctx.config(); cfg && !(m_before == *cfg))
			{
				ctx.ops.Submit(std::make_unique<SetRenderConfigOp>(m_before, *cfg));
			}
		}
		m_editing = false;
	});
}

} // namespace neurus
