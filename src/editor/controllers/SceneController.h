/**
 * @file SceneController.h
 * @brief Scene document controller - selection and membership, event-driven.
 *
 * Owns what the scene document CONTAINS and what is selected in it: add,
 * delete, select, deselect and camera activation. Object property writes
 * (visibility, transforms, camera / mesh / light / environment / debug knobs)
 * belong to ObjectController, which resolves through the resource pool rather
 * than the scene.
 *
 * Fully stateless: all handlers are free functions in the .cpp; every event
 * carries an integer UID, resolved against the current scene through the
 * context, so a scene swap can never leave a handler holding a stale object.
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
	 * @brief Subscribes to scene-document events on the given context.
	 *
	 * Registers lambda handlers that forward each event to the corresponding
	 * free-function handler; selection and membership handlers capture the
	 * before state, mutate the scene, then record an operation via the sink.
	 *
	 * @param ctx Controller context (events, resources, ops, scene, viewport).
	 */
	void Init(ControllerContext& ctx) override;
};

} // namespace neurus
