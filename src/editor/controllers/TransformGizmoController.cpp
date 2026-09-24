/**
 * @file TransformGizmoController.cpp
 * @brief Event handlers for the modal G / R / S transform gesture.
 *
 * Stateless -- all handlers are free functions in an anonymous namespace, the
 * SceneController shape. Each one resolves the gesture's object uid against the
 * *current* Scene, calls exactly one TransformGizmo method, and enqueues the
 * notifications that mutation implies. No gesture bookkeeping lives here.
 *
 * ## Two notification tiers, not one
 *
 * A drag and a confirm are different events for the GPU and for the project:
 *
 *   - Dragged()   -> LightGpuChanged (a light only) + RenderResetEvent.
 *                    Cheap, per frame. It does NOT mark the project modified:
 *                    the gesture may still be cancelled, and a cancelled gesture
 *                    must leave no trace at all -- which is why the cancel path
 *                    also goes through Dragged() and not through Confirmed().
 *   - Confirmed() -> LightingRebuild (a light only) + SceneModified +
 *                    RenderResetEvent. The same end state a PropertyPanel edit
 *                    would leave, because SceneController's own transform
 *                    handlers rebuild the whole light dict.
 *
 * A light needs either tier because the drag writes Transform3D only, while the
 * light's GPU position lives in an SSBO struct nothing else refreshes.
 *
 * ## RenderResetEvent is also the repaint
 *
 * Every handler enqueues it, including the cancel path -- Editor.cpp's
 * RenderResetEvent subscription is what marks the overlay builders dirty, so this
 * is how the guide geometry appears, follows the cursor and disappears again. A
 * handler that mutates nothing still enqueues it when the *guide* changed.
 */

#include "editor/controllers/TransformGizmoController.h"

#include <memory>

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
 * @brief Resolves @p uid against the current Scene, the SceneController pattern.
 *
 * Re-resolved on every event rather than cached: an object can be deleted
 * mid-gesture, and EventQueue::Process() drains re-entrant enqueues in the same
 * call, so a pointer latched at Arm() could already dangle by the next handler.
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

bool IsLight(const neurus::ObjectID& obj)
{
	return obj.o_type == neurus::ObjectID::GOType::GO_LIGHT ||
	       obj.o_type == neurus::ObjectID::GOType::GO_POLYLIGHT;
}

// ---------------------------------------------------------------------------
// Emit helpers -- see the file comment for why there are two
// ---------------------------------------------------------------------------

/** @brief A live, still-revertible mutation: GPU sync + repaint, no dirty flag. */
void Dragged(neurus::ObjectID& obj, neurus::IEventQueue& events)
{
	if (IsLight(obj))
		events.enqueue(neurus::LightGpuChanged{obj.GetObjectID()});
	events.enqueue(neurus::RenderResetEvent{});
}

/** @brief The gesture was kept: full GPU sync + project dirty + repaint. */
void Confirmed(neurus::ObjectID& obj, neurus::IEventQueue& events)
{
	if (IsLight(obj))
		events.enqueue(neurus::LightingRebuild{});
	events.enqueue(neurus::SceneModified{});
	events.enqueue(neurus::RenderResetEvent{});
}

// ---------------------------------------------------------------------------
// Undo: exactly one op for the whole gesture
// ---------------------------------------------------------------------------

/**
 * @brief Submits the one op spanning the gesture, or none when nothing moved.
 *
 * The mode picks both the op type and which snapshot component to compare. A
 * gesture that ended where it started must record nothing -- otherwise a stray
 * G-then-Enter would litter the undo stack with no-ops.
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
 * the move half-applied. The live gesture's uid is re-resolved because it need not
 * be the object this request names.
 *
 * The retained cursor is handed in as the free gesture's anchor — bare G and bare S
 * are live from this moment, so the pixel the key was pressed at is what the very next
 * mouse move measures against. Same read, and same reason, as OnAxisRequested below.
 */
void OnModeRequested(const neurus::GizmoModeRequested& e, const neurus::ControllerContext& ctx)
{
	neurus::TransformGizmo* gizmo = ctx.gizmo();
	if (!gizmo) return;

	if (gizmo->IsActive())
	{
		const Target prev = Resolve(ctx, gizmo->ObjectUid());
		if (prev.Valid())
		{
			gizmo->Cancel(*prev.transform);
			Dragged(*prev.obj, ctx.events);
		}
		gizmo->Disarm();
	}

	const neurus::EditorViewport* vp = ctx.viewport();
	const Target target = Resolve(ctx, e.objectUid);
	if (vp && target.Valid())
		gizmo->Arm(e.mode, e.objectUid, *target.transform, vp->Cursor());

	// Unconditional: the guide either appeared (armed) or vanished (the cancel above).
	ctx.events.enqueue(neurus::RenderResetEvent{});
}

/**
 * @brief X / Y / Z: constrain the live gesture, then normalize back to the before-state.
 */
void OnAxisRequested(const neurus::GizmoAxisRequested& e, const neurus::ControllerContext& ctx)
{
	neurus::TransformGizmo* gizmo = ctx.gizmo();
	if (!gizmo || !gizmo->IsActive()) return;

	const neurus::EditorViewport* vp = ctx.viewport();
	if (!vp) return;

	const Target target = Resolve(ctx, gizmo->ObjectUid());
	if (!target.Valid()) return;

	// A key press carries no cursor, so the retained one is the only anchor there is.
	// The Editor pushes it on every mouse move, unconditionally and ahead of the
	// camera-orbit gate, precisely so this read is current.
	const glm::vec2 cursor = vp->Cursor();

	if (!gizmo->Constrain(e.axis, *vp, cursor))
		return;  // refused (axis within ~1.8 deg of the view ray): stay armed, axis-less

	// Constrain re-latched every anchor at `cursor`, so this one drag reproduces the
	// before-state exactly (t - grabAnchorT == 0, accumAngle == 0, factor == 1). That
	// is what makes switching axis mid-gesture *undo* the previous axis's partial
	// transform rather than compound with it.
	gizmo->Drag(*vp, cursor, *target.transform);
	Dragged(*target.obj, ctx.events);
}

/** @brief Cursor motion drives the drag. Dropped whole when no gesture is live. */
void OnMouseMove(const neurus::MouseMoveEvent& e, const neurus::ControllerContext& ctx)
{
	neurus::TransformGizmo* gizmo = ctx.gizmo();
	if (!gizmo || !gizmo->IsActive()) return;

	const neurus::EditorViewport* vp = ctx.viewport();
	if (!vp) return;

	const Target target = Resolve(ctx, gizmo->ObjectUid());
	if (!target.Valid()) return;

	// e.position, never vp->Cursor(): the Editor's own subscription is what pushes the
	// cursor into the viewport, and dispatch is registration-ordered, so reading it
	// back here would silently consume a one-frame-stale cursor if the two
	// subscriptions were ever registered the other way round.
	if (!gizmo->Drag(*vp, e.position, *target.transform))
		return;  // nothing moved -- no GPU sync, no repaint

	Dragged(*target.obj, ctx.events);
}

/** @brief LMB / Return: keep the transform, record one op, disarm. */
void OnConfirmed(const neurus::ControllerContext& ctx)
{
	neurus::TransformGizmo* gizmo = ctx.gizmo();
	if (!gizmo || !gizmo->IsActive()) return;

	const Target target = Resolve(ctx, gizmo->ObjectUid());
	if (target.Valid())
	{
		SubmitGesture(*gizmo, *target.obj, *target.transform, ctx);
		Confirmed(*target.obj, ctx.events);
	}

	gizmo->Disarm();
	ctx.events.enqueue(neurus::RenderResetEvent{});  // the guide has to disappear
}

/** @brief Esc / RMB / focus loss: restore the snapshot, disarm, record nothing. */
void OnAborted(const neurus::ControllerContext& ctx)
{
	neurus::TransformGizmo* gizmo = ctx.gizmo();
	if (!gizmo || !gizmo->IsActive()) return;

	const Target target = Resolve(ctx, gizmo->ObjectUid());
	if (target.Valid())
	{
		gizmo->Cancel(*target.transform);
		Dragged(*target.obj, ctx.events);  // deliberately not Confirmed(): net zero change
	}

	gizmo->Disarm();
	ctx.events.enqueue(neurus::RenderResetEvent{});
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
