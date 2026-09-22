/**
 * @file TransformGizmoController.h
 * @brief Drives the modal transform gesture from viewport input events.
 *
 * Stateless beyond what TransformGizmo already holds: every handler resolves the
 * object uid against the current Scene, calls exactly one state-machine method, and
 * enqueues the resulting notifications. There is no gesture bookkeeping here — that
 * is the state machine's job — which is why this class has one method.
 *
 * Event mapping:
 *   - GizmoModeRequested  -> Cancel a live gesture, then Arm the new mode
 *   - GizmoAxisRequested  -> Constrain (may refuse), then re-derive the before-state
 *   - MouseMoveEvent      -> Drag, using e.position
 *   - GizmoConfirmed      -> Submit exactly one op, Disarm
 *   - GizmoCancelled      -> Cancel, Disarm. No op recorded.
 *   - ViewportFocusLost   -> same as GizmoCancelled
 *
 * ## One undo entry per gesture
 *
 * The drag mutates Transform3D **directly** and submits nothing; confirm submits one
 * SetPositionOp / SetRotationOp / SetScaleOp for the whole gesture. This is the
 * CameraController pattern, sanctioned at operation-system.instructions.md:86-108.
 * Routing the drag through PositionChanged / RotationChanged / ScaleChanged instead
 * would record ~60 undo entries per second: SceneController submits on every one of
 * those events and those three ops declare no MergeKey(), so nothing coalesces.
 *
 * Because the drag bypasses SceneController, this controller must emit
 * SceneModified + RenderResetEvent itself — and LightGpuChanged for a light, whose
 * GPU position lives in an SSBO the transform write does not touch.
 *
 * ## Registration order matters twice
 *
 * Registered **after** CameraController so its handlers see an already-updated
 * camera. And it reads the cursor from MouseMoveEvent::position rather than from
 * EditorViewport::Cursor(), because the Editor's own subscription is what pushes the
 * cursor into the viewport and EventQueue dispatch is registration-ordered: reading
 * it back would silently consume a one-frame-stale cursor if the two subscriptions
 * were ever registered the other way round.
 */

#pragma once

#include "editor/controllers/Controllers.h"

namespace neurus {

/**
 * @brief Event handlers for the modal G / R / S transform gesture.
 *
 * Owns no state: the gesture lives in the Editor-owned TransformGizmo reached
 * through ControllerContext::gizmo.
 */
class TransformGizmoController : public Controllers
{
public:
	TransformGizmoController() = default;

	/**
	 * @brief Subscribes to the six gizmo/input events listed in the file comment.
	 *
	 * Must be called after CameraController::Init() so a drag handler sees the
	 * camera pose this frame's orbit already produced.
	 */
	void Init(ControllerContext& ctx) override;
};

} // namespace neurus
