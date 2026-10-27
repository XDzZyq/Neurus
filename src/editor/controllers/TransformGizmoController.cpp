/**
 * @file TransformGizmoController.cpp
 * @brief Event handlers for the modal G / R / S transform gesture.
 *
 * All handlers are free functions. Each resolves the gesture's object uid
 * against the *current* scene, calls one TransformGizmo method, and emits the
 * notifications that mutation implies - through neurus::notify, shared with the
 * scene mutation path so the two cannot drift. A live drag is a preview (no
 * dirty mark); a confirm is committed.
 *
 * Every handler emits a repaint, including the cancel path, because that is
 * also what makes the guide geometry appear, follow the cursor and disappear.
 */

#include "editor/controllers/TransformGizmoController.h"

#include <memory>

#include "editor/controllers/MutationNotify.h"
#include "editor/events/EditorEvents.h"
#include "editor/events/GizmoEvents.h"
#include "editor/events/InputEvents.h"
#include "editor/operations/SceneOperations.h"
#include "editor/viewport/EditorViewport.h"
#include "editor/viewport/TransformGizmo.h"

#include "scene/ObjectID.h"
#include "scene/Scene.h"
#include "scene/Transform.h"

namespace {

// ---------------------------------------------------------------------------
// Target resolution
// ---------------------------------------------------------------------------

/// @brief A gesture target resolved from a uid: the identity and its transform.
struct Target
{
	neurus::ObjectID*    obj = nullptr;
	neurus::Transform3D* transform = nullptr;

	bool Valid() const { return obj != nullptr && transform != nullptr; }
};

/**
 * @brief Resolves @p uid against the current Scene.
 *
 * Re-resolved on every event rather than cached: an object can be deleted
 * mid-gesture, so a pointer latched at Arm() could already dangle.
 */
Target Resolve(const neurus::ControllerContext& ctx, int uid)
{
	Target target{};

	neurus::Scene* scene = ctx.scene();
	if (!scene) return target;
	neurus::ObjectID* obj = scene->GetObjectID(uid);
	if (!obj) return target;
	void* transformPtr = obj->GetTransform();
	if (!transformPtr) return target;

	target.obj = obj;
	target.transform = static_cast<neurus::Transform3D*>(transformPtr);
	return target;
}

// ---------------------------------------------------------------------------
// Undo: exactly one op for the whole gesture
// ---------------------------------------------------------------------------

/**
 * @brief Submits the one op spanning the gesture, or none when nothing moved.
 *
 * A gesture that ended where it started must record nothing, so a stray
 * G-then-Enter cannot litter the undo stack with no-ops.
 */
void SubmitGesture(const neurus::TransformGizmo& gizmo, neurus::ObjectID& obj,
                   const neurus::Transform3D& transform, const neurus::ControllerContext& ctx)
{
	const neurus::TransformSnapshot& before = gizmo.Before();
	const int uid = obj.GetObjectID();

	switch (gizmo.Mode())
	{
	case neurus::GizmoMode::Move:
		if (transform.GetPosition() != before.position)
			ctx.ops.Submit(std::make_unique<neurus::SetPositionOp>(
			    uid, before.position, transform.GetPosition()));
		break;
	case neurus::GizmoMode::Rotate:
		if (transform.GetRotation() != before.rotation)
			ctx.ops.Submit(std::make_unique<neurus::SetRotationOp>(
			    uid, before.rotation, transform.GetRotation()));
		break;
	case neurus::GizmoMode::Scale:
		if (transform.GetScale() != before.scale)
			ctx.ops.Submit(std::make_unique<neurus::SetScaleOp>(
			    uid, before.scale, transform.GetScale()));
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

/**
 * @brief G / R / S: discard any live gesture, then arm the requested mode.
 *
 * Re-arming cancels first, as Blender does: pressing R during a G must not leave
 * the move half-applied. The retained cursor becomes the free gesture's anchor,
 * so the very next mouse move measures against the pixel the key was pressed at.
 */
void OnModeRequested(const neurus::GizmoModeRequested& e, const neurus::ControllerContext& ctx)
{
	// The gizmo is reached through its owner, which is also where the anchor cursor
	// comes from: one handle, so the two cannot disagree about which viewport.
	neurus::EditorViewport* vp = ctx.viewport();
	neurus::TransformGizmo* gizmo = vp ? &vp->Gizmo() : nullptr;
	if (!gizmo) return;

	if (gizmo->IsActive())
	{
		const Target prev = Resolve(ctx, gizmo->ObjectUid());
		if (prev.Valid())
		{
			gizmo->Cancel(*prev.transform);
			neurus::notify::ObjectPreviewed(*prev.obj, ctx.events);
		}
		gizmo->Disarm();
	}

	const Target target = Resolve(ctx, e.objectUid);
	if (target.Valid())
		gizmo->Arm(e.mode, e.objectUid, *target.transform, vp->Cursor());

	// Unconditional: the guide either appeared (armed) or vanished (the cancel above).
	neurus::notify::Repaint(ctx.events);
}

/**
 * @brief X / Y / Z / W: set the live gesture's constraint, then normalize back to
 *        the before-state.
 *
 * W arrives as GizmoAxis::None, which Constrain() reads as "screen space": it
 * drops the latched axis and re-latches the free anchors, so leaving a constraint
 * is as exact as entering one.
 */
void OnAxisRequested(const neurus::GizmoAxisRequested& e, const neurus::ControllerContext& ctx)
{
	neurus::EditorViewport* vp = ctx.viewport();
	neurus::TransformGizmo* gizmo = vp ? &vp->Gizmo() : nullptr;
	if (!gizmo || !gizmo->IsActive()) return;

	const Target target = Resolve(ctx, gizmo->ObjectUid());
	if (!target.Valid()) return;

	// A key press carries no cursor, so the retained one is the only anchor there
	// is; the Editor pushes it on every mouse move precisely so this read is current.
	const glm::vec2 cursor = vp->Cursor();

	if (!gizmo->Constrain(e.axis, *vp, cursor))
		return;  // refused (axis within ~1.8 deg of the view ray): stay armed, axis-less

	// Constrain re-latched every anchor at `cursor`, so this one drag reproduces the
	// before-state exactly. That is what makes an axis switch mid-gesture *undo* the
	// previous axis's partial transform rather than compound with it.
	gizmo->Drag(*vp, cursor, *target.transform);
	neurus::notify::ObjectPreviewed(*target.obj, ctx.events);
}

/** @brief Cursor motion drives the drag. Dropped whole when no gesture is live. */
void OnMouseMove(const neurus::MouseMoveEvent& e, const neurus::ControllerContext& ctx)
{
	neurus::EditorViewport* vp = ctx.viewport();
	neurus::TransformGizmo* gizmo = vp ? &vp->Gizmo() : nullptr;
	if (!gizmo || !gizmo->IsActive()) return;

	const Target target = Resolve(ctx, gizmo->ObjectUid());
	if (!target.Valid()) return;

	// e.position, never vp->Cursor(): the Editor's own subscription is what pushes
	// the cursor into the viewport, and dispatch is registration-ordered, so reading
	// it back here would silently consume a one-frame-stale cursor.
	if (!gizmo->Drag(*vp, e.position, *target.transform))
		return;  // nothing moved -- no GPU sync, no repaint

	neurus::notify::ObjectPreviewed(*target.obj, ctx.events);
}

/** @brief LMB / Return: keep the transform, record one op, disarm. */
void OnConfirmed(const neurus::ControllerContext& ctx)
{
	neurus::EditorViewport* vp = ctx.viewport();
	neurus::TransformGizmo* gizmo = vp ? &vp->Gizmo() : nullptr;
	if (!gizmo || !gizmo->IsActive()) return;

	const Target target = Resolve(ctx, gizmo->ObjectUid());
	if (target.Valid())
	{
		SubmitGesture(*gizmo, *target.obj, *target.transform, ctx);
		neurus::notify::ObjectMutated(*target.obj, ctx.events);
	}

	gizmo->Disarm();
	neurus::notify::Repaint(ctx.events);  // the guide has to disappear
}

/** @brief Esc / RMB / focus loss: restore the snapshot, disarm, record nothing. */
void OnAborted(const neurus::ControllerContext& ctx)
{
	neurus::EditorViewport* vp = ctx.viewport();
	neurus::TransformGizmo* gizmo = vp ? &vp->Gizmo() : nullptr;
	if (!gizmo || !gizmo->IsActive()) return;

	const Target target = Resolve(ctx, gizmo->ObjectUid());
	if (target.Valid())
	{
		gizmo->Cancel(*target.transform);
		// The preview tier on purpose: the snapshot is back, so the net change is
		// zero and the project must not end up dirty for it.
		neurus::notify::ObjectPreviewed(*target.obj, ctx.events);
	}

	gizmo->Disarm();
	neurus::notify::Repaint(ctx.events);
}

} // namespace

namespace neurus {

void TransformGizmoController::Init(ControllerContext& ctx)
{
	// The context is captured BY VALUE so the stored handler lambdas are fully
	// self-contained (the context's references + providers copy cheaply).
	ctx.events.subscribe<GizmoModeRequested>(
	    [ctx](const GizmoModeRequested& e) { OnModeRequested(e, ctx); });
	ctx.events.subscribe<GizmoAxisRequested>(
	    [ctx](const GizmoAxisRequested& e) { OnAxisRequested(e, ctx); });
	ctx.events.subscribe<MouseMoveEvent>(
	    [ctx](const MouseMoveEvent& e) { OnMouseMove(e, ctx); });
	ctx.events.subscribe<GizmoConfirmed>(
	    [ctx](const GizmoConfirmed&) { OnConfirmed(ctx); });
	ctx.events.subscribe<GizmoCancelled>(
	    [ctx](const GizmoCancelled&) { OnAborted(ctx); });
	ctx.events.subscribe<ViewportFocusLost>(
	    [ctx](const ViewportFocusLost&) { OnAborted(ctx); });
}

} // namespace neurus
