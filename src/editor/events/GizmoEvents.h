/**
 * @file GizmoEvents.h
 * @brief Typed intents for the modal transform gizmo (move / rotate / scale).
 *
 * These are *intents*, not state changes: the Editor translates a viewport
 * keystroke into one of them and emits it unconditionally, and the controller
 * decides whether it means anything. Only the gesture-opening event carries an
 * object uid, a stable identity the controller resolves at dispatch time; the
 * later events need none, since the controller already holds the latched target.
 *
 * Cursor motion is not an event of its own: the controller subscribes to the
 * existing mouse-move event and ignores it while no gesture is active.
 */

#pragma once

namespace neurus {

/**
 * @brief Which transform a modal gesture is manipulating.
 *
 * Shared vocabulary for the events below and for the state machine that
 * consumes them.
 */
enum class GizmoMode
{
	None = 0,  ///< No modal gesture active.
	Move,      ///< G - translate along a constraint axis.
	Rotate,    ///< R - rotate about a gimbal axis.
	Scale      ///< S - scale along a local axis.
};

/**
 * @brief Which axis a modal gesture is constrained to.
 *
 * The axis this names is mode-dependent: Rotate uses the gimbal axes (the
 * one-parameter subgroups of the stored Euler triple) while Move and Scale use
 * the true local axes. The two sets share Y always, X only when roll == 0, and Z
 * only when pitch and roll are both 0.
 */
enum class GizmoAxis
{
	/**
	 * @brief Unconstrained - the *screen-space* variant of whichever mode is armed.
	 *
	 * Not an absence of gesture: it means the view plane for Move, uniform for
	 * Scale and the view axis for Rotate, both for a bare G / R / S and for W.
	 */
	None = 0,
	X,
	Y,
	Z
};

/**
 * @brief User pressed G / R / S: arm a modal gesture on the active object.
 *
 * Arming alone applies no transform; it shows the candidate axes. Re-arming
 * while a gesture is live switches mode and restores the before-state first.
 */
struct GizmoModeRequested
{
	int       objectUid = 0;                ///< Target object UID (0 = nothing selected; dropped).
	GizmoMode mode      = GizmoMode::None;  ///< Requested mode.
};

/**
 * @brief User pressed X / Y / Z / W: set the live gesture's constraint.
 *
 * Dropped when no gesture is active. Pressing the same axis twice re-latches the
 * anchor; W requests GizmoAxis::None, the screen-space variant.
 */
struct GizmoAxisRequested
{
	GizmoAxis axis = GizmoAxis::None;  ///< Requested constraint; None = screen space.
};

/**
 * @brief User confirmed the gesture (LMB or Return).
 *
 * The controller keeps the mutated transform and submits exactly one operation
 * spanning the whole gesture - or none when nothing actually moved.
 */
struct GizmoConfirmed
{
};

/**
 * @brief User cancelled the gesture (Esc, RMB, or viewport focus loss).
 *
 * The controller restores the snapshot taken when the gesture began and submits
 * nothing, so a cancelled gesture leaves no undo entry behind.
 */
struct GizmoCancelled
{
};

} // namespace neurus
