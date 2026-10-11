/**
 * @file RenderConfigController.h
 * @brief Render-config controller - applies + records whole-config edits.
 *
 * The single mutation path for RenderConfig: it writes the new config into the
 * Editor-owned instance reached through the context's config provider, so it
 * holds no reference to the config.
 *
 * Undo bounding mirrors a camera drag gesture: a slider drag brackets its stream
 * of changes with ConfigEditBegin/ConfigEditEnd and collapses to one op recorded
 * on release; discrete edits record immediately, one op each.
 */

#pragma once

#include <functional>

#include "editor/controllers/Controllers.h"
#include "core/RenderConfig.h"

namespace neurus {

/**
 * @brief Event-driven controller that applies and records RenderConfig edits.
 */
class RenderConfigController : public Controllers
{
public:
	/** @brief Constructs the controller. No stored references: the live config
	 *         is reached through the ControllerContext passed to Init(). */
	RenderConfigController() = default;

	/**
	 * @brief Subscribes to config change + edit-gesture events.
	 * @param ctx Controller context (events, resources, ops, scene, config).
	 */
	void Init(ControllerContext& ctx) override;

private:
	bool m_editing = false;      ///< True between ConfigEditBegin and ConfigEditEnd.
	RenderConfig m_before{};     ///< Config captured at gesture start (undo endpoint).
};

} // namespace neurus
