/**
 * @file ObjectController.h
 * @brief Pooled-object property controller - event-driven, no per-frame polling.
 *
 * Applies property edits (visibility, transforms, camera / mesh / light /
 * environment / debug knobs) to the object named by each event's integer UID.
 * Resolution goes through the RESOURCE POOL, never through the scene: an object
 * being writable is a property of the pool, not of scene membership, so a
 * pooled object a script owns is writable before it enters the document - and
 * the viewport camera, which is pooled but is not scene content, stays editable.
 *
 * Scene membership (add / delete), selection and camera activation are
 * SceneController's; this controller never reads the scene.
 *
 * Fully stateless: all handlers are free functions in the .cpp; every event
 * carries an integer UID re-resolved against the pool at dispatch, so no
 * handler can hold a stale object.
 */

#pragma once

#include "editor/controllers/Controllers.h"

namespace neurus {

class ObjectController : public Controllers
{
public:
	/**
	 * @brief Constructs the controller. No stored references: everything is
	 *        reached through the ControllerContext passed to Init().
	 */
	ObjectController() = default;

	/**
	 * @brief Subscribes to pooled-object property events on the given context.
	 *
	 * Registers lambda handlers that forward each event to the corresponding
	 * free-function handler; each handler captures the before value, mutates
	 * through the pool, then records a forward operation via the sink and
	 * routes the consequences through neurus::notify.
	 *
	 * @param ctx Controller context (events, resources, ops; scene unused).
	 */
	void Init(ControllerContext& ctx) override;
};

} // namespace neurus
