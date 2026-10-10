/**
 * @file RenderConfigController.h
 * @brief Render-config controller — applies + records whole-config edits.
 *
 * RenderConfigController is the single mutation path for RenderConfig. It
 * subscribes to RenderConfigChangedEvent (emitted by the UI panel and replayed
 * by SetRenderConfigOp) and writes the new config into the Editor-owned
 * RenderConfig, reached through the ControllerContext's config provider — so
 * the controller holds no reference to the config (or any context member).
 *
 * Undo bounding mirrors the camera drag gesture: a slider drag brackets its
 * stream of value changes with ConfigEditBegin / ConfigEditEnd, so the whole
 * drag collapses to a single SetRenderConfigOp recorded on release. Discrete
 * edits (checkbox, combo box) arrive without a gesture and are recorded
 * immediately, one op each.
 *
 * @note RenderConfig is a scene-level (not pooled) Editor singleton, so it is
 *       reached through the context's config provider rather than the pool.
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
