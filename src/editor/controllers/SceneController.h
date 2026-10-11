/**
 * @file SceneController.h
 * @brief Scene mutation controller - event-driven, no per-frame polling.
 *
 * Translates scene-domain events (selection, transform, visibility, property
 * edits) into scene object mutations. Fully stateless: all handlers are free
 * functions in the .cpp; every event carries an integer UID, resolved against
 * the current scene through the context, so a scene swap can never leave a
 * handler holding a stale object.
 *
 * GPU uploads stay out of here: the controller emits typed change events that
 * the Editor subscribes to.
 */

#pragma once

#include "editor/controllers/Controllers.h"

namespace neurus {

class SceneController : public Controllers
{
public:
	/**
	 * @brief Constructs the controller. No stored references: everything is
	 *        reached through the ControllerContext passed to Init().
	 */
	SceneController() = default;

	/**
	 * @brief Subscribes to scene events on the given context.
	 *
	 * Registers lambda handlers that forward each event to the corresponding
	 * free-function handler; property/transform handlers capture the before
	 * value, mutate, then record a forward operation via the sink.
	 *
	 * @param ctx Controller context (events, resources, ops, scene).
	 */
	void Init(ControllerContext& ctx) override;
};

} // namespace neurus
