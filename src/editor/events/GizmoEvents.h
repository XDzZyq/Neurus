/**
 * @file GizmoEvents.h
 * @brief Typed intents for the modal transform gizmo (move / rotate / scale).
 *
 * These are *intents*, not state changes: the Editor translates a viewport
 * keystroke into one of them and enqueues it unconditionally, and
 * TransformGizmoController decides whether it means anything. That split is
 * deliberate — the Editor must never read gizmo state to decide whether to
 * enqueue, because EventQueue::Process() is FIFO and drains re-entrant enqueues
 * in the same call, so a gate in the Editor would silently swallow an axis key
 * that arrived in the same frame as its mode key.
 *
 * Only the gesture-opening event carries an object uid, following CameraEvents.h:
 * a uid is a stable identity where a raw pointer is not, and the controller
 * resolves it against the current scene through ControllerContext at dispatch
 * time. The later events need none — the controller already holds the target it
 * latched when the gesture began.
 *
 * Cursor motion is not an event of its own: the controller subscribes to the
 * existing MouseMoveEvent and ignores it while no gesture is active.
 */

#pragma once

namespace neurus {

/**
 * @brief Which transform a modal gesture is manipulating.
 *
 * Shared vocabulary for the events below and for the gizmo state machine that
 * consumes them, which is why it lives in this leaf header rather than in
 * TransformGizmo.h — an event header must not depend on the controller's types.
 */
enum class GizmoMode
{
	None = 0,  ///< No modal gesture active.
	Move,      ///< G — translate along a constraint axis.
	Rotate,    ///< R — rotate about a gimbal axis.
	Scale      ///< S — scale along a local axis.
};

/**
 * @brief Which axis a modal gesture is constrained to.
 *
 * The axis this names is mode-dependent, and that is not an inconsistency: the
 * model matrix is T * Rz(yaw) * Rx(pitch) * Ry(roll) * S, so Rotate must use the
 * gimbal axes (the one-parameter subgroups of the stored Euler triple) while Move
 * and Scale must use the true local axes (the columns of R). They coincide only
 * when roll == 0. See TransformGizmo for the derivation.
 */
enum class GizmoAxis
{
	None = 0,  ///< Armed but unconstrained; no transform is applied yet.
	X,
	Y,
	Z
};

/**
 * @brief User pressed G / R / S: arm a modal gesture on the active object.
 *
 * Arming alone applies no transform — it shows the candidate axes and waits for a
 * GizmoAxisRequested. Re-arming while a gesture is live switches mode and restores
 * the before-state first, so a mistyped G-then-R leaves nothing half-applied.
 */
struct GizmoModeRequested
{
	int       objectUid = 0;                ///< Target object UID (0 = nothing selected; dropped).
	GizmoMode mode      = GizmoMode::None;  ///< Requested mode.
};

/**
 * @brief User pressed X / Y / Z: constrain the live gesture to one axis.
 *
 * Dropped when no gesture is active. Pressing the same axis twice is not a toggle
 * in wave 1; it simply re-latches the anchor.
 */
struct GizmoAxisRequested
{
	GizmoAxis axis = GizmoAxis::None;  ///< Requested constraint axis.
};

/**
 * @brief User confirmed the gesture (LMB or Return).
 *
 * The controller keeps the mutated transform and Submits exactly one
 * SetPosition/SetRotation/SetScale operation spanning the whole gesture — or none
 * at all when nothing actually moved.
 */
struct GizmoConfirmed
{
};

/**
 * @brief User cancelled the gesture (Esc, RMB, or viewport focus loss).
 *
 * The controller restores the snapshot it took when the gesture began and Submits
 * nothing, so a cancelled gesture leaves no undo entry behind.
 */
struct GizmoCancelled
{
};

} // namespace neurus
