/**
 * @file TransformGizmoController.h
 * @brief Drives the modal transform gesture from viewport input events.
 *
 * Stateless beyond what the state machine already holds: every handler resolves
 * the object uid against the current scene, calls exactly one TransformGizmo
 * method, and emits the notifications that mutation implies.
 *
 * The drag mutates the transform directly and submits nothing; confirm submits
 * one op for the whole gesture. Because the drag bypasses the scene-mutation
 * path, this controller emits the dirty/GPU/repaint notifications itself.
 *
 * Registered AFTER CameraController so its handlers see an already-updated
 * camera, and it reads the cursor from the event rather than from the viewport,
 * since dispatch is registration-ordered.
 */

#pragma once

#include "editor/controllers/Controllers.h"

namespace neurus {

/**
 * @brief Event handlers for the modal G / R / S transform gesture.
 *
 * Owns no state: the gesture lives in the TransformGizmo owned by the Editor's
 * viewport, reached as `ControllerContext::viewport()->Gizmo()`.
 */
class TransformGizmoController : public Controllers
{
public:
	TransformGizmoController() = default;

	/**
	 * @brief Subscribes to the gizmo and input events that drive the gesture.
	 *
	 * Must be called after CameraController::Init() so a drag handler sees the
	 * camera pose this frame's orbit already produced.
	 */
	void Init(ControllerContext& ctx) override;
};

} // namespace neurus
