/**
 * @file SceneController.cpp
 * @brief Event-driven scene document handlers: selection and membership.
 *
 * This controller owns what the scene DOCUMENT contains and what is selected in
 * it - add, delete, select, deselect and camera activation. Object property
 * writes (visibility, transforms, camera / mesh / light / environment / debug
 * knobs) live in ObjectController, which resolves through the resource pool
 * instead of the scene, so a property edit does not require scene membership.
 *
 * Every handler resolves an integer UID against the current scene and routes the
 * consequences through neurus::notify - the shared mutation policy - rather than
 * deciding them locally.
 */

#include "editor/controllers/SceneController.h"

#include <vector>

#include "editor/controllers/MutationNotify.h"
#include "editor/events/CameraEvents.h"
#include "editor/events/SceneEvents.h"
#include "editor/events/EditorEvents.h"
#include "editor/operations/SceneOperations.h"
#include "editor/Input.h"
#include "editor/viewport/EditorViewport.h"

#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "scene/ObjectID.h"

namespace {

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

/** @brief Captures the current selection set as an absolute UID endpoint. */
neurus::SelectionState SnapshotSelection(const neurus::Scene& scene)
{
	neurus::SelectionState state;
	neurus::SnapshotSelectionUids(scene.selections, state.selectedUids, state.activeUid);
	return state;
}

/** @brief Records a selection edit if it changed the set (keeps redo intact). */
void RecordSelection(neurus::Scene& scene, neurus::SelectionState before, neurus::IOperationSink& ops)
{
	neurus::SelectionState after = SnapshotSelection(scene);
	if (before.selectedUids == after.selectedUids && before.activeUid == after.activeUid)
		return; // No-op edit (e.g. re-select active object): nothing to record.
	ops.Submit(std::make_unique<neurus::SetSelectionOp>(std::move(before), std::move(after)));
}

void OnObjectSelected(const neurus::ObjectSelected& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;

	const bool increment = (e.modifiers & (neurus::Input::Mod_Shift | neurus::Input::Mod_Ctrl)) != 0;

	neurus::SelectionState before = SnapshotSelection(*scene);

	if (e.objectUid == 0)
	{
		// Background click (objectUid 0) -> clear selection
		if (!increment) scene->selections.ClearSelection();
	}
	else if (neurus::ObjectID* obj = scene->GetObjectID(e.objectUid))
	{
		scene->selections.Select(obj, increment);
	}

	RecordSelection(*scene, std::move(before), ctx.ops);
}

void OnObjectDeselected(const neurus::ObjectDeselected& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	neurus::ObjectID* obj = scene->GetObjectID(e.objectUid);
	if (!obj) return;

	neurus::SelectionState before = SnapshotSelection(*scene);
	scene->selections.Deselect(obj, false);
	RecordSelection(*scene, std::move(before), ctx.ops);
}

void OnSelectionChanged(const neurus::SelectionChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;

	// Replay path for SetSelectionOp: resolve stored UIDs to live objects and
	// restore the whole set at once. Skip stale UIDs (object gone).
	neurus::RestoreSelectionUids(*scene, e.selectedUids, e.activeUid);
}

// ---------------------------------------------------------------------------
// Active camera
// ---------------------------------------------------------------------------

/**
 * @brief Activates a scene camera (or deactivates with uid 0) as ONE undo entry.
 *
 * Gesture and replay path both. The op is recorded only when the value actually
 * changes. Activating a UID that is not scene content is refused, which keeps the
 * viewport camera unactivatable. The newly activated camera needs the viewport's
 * extent, so a resize event is emitted for it.
 */
void OnActiveCameraChanged(const neurus::ActiveCameraChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;

	const int before = scene->ActiveCameraID();
	if (e.camUid == 0)
		scene->DeactivateCamera();
	else if (!scene->ActivateCamera(e.camUid))
		return; // not a scene camera: Scene logged it, nothing changed

	const int after = scene->ActiveCameraID();
	if (after == before) return; // no-op toggle: record nothing, emit nothing

	ctx.ops.Submit(std::make_unique<neurus::SetActiveCameraOp>(before, after));

	if (after != 0)
	{
		if (const neurus::EditorViewport* vp = ctx.viewport())
		{
			const glm::uvec2 size = vp->Size();
			if (size.x != 0 && size.y != 0)
			{
				ctx.events.Emit(neurus::CameraResizeEvent{
					after, static_cast<int>(size.x), static_cast<int>(size.y)});
			}
		}
	}

	neurus::notify::Mutated(ctx.events);
}

// ---------------------------------------------------------------------------
// Scene membership (Add / Delete)
// ---------------------------------------------------------------------------

/** @brief Registers a pooled object into the scene by type (camera/mesh/light/env). */
void UsePooledObject(neurus::Scene& scene, neurus::IResourceLookup& resources, int uid)
{
	auto obj = resources.Get<neurus::ObjectID>(uid);
	if (!obj) return;
	switch (obj->o_type)
	{
	case neurus::ObjectID::GOType::GO_CAM:
		scene.UseCamera(resources.Get<neurus::Camera>(uid));
		break;
	case neurus::ObjectID::GOType::GO_MESH:
		scene.UseMesh(resources.Get<neurus::Mesh>(uid));
		break;
	case neurus::ObjectID::GOType::GO_LIGHT:
	case neurus::ObjectID::GOType::GO_POLYLIGHT:
		scene.UseLight(resources.Get<neurus::Light>(uid));
		break;
	case neurus::ObjectID::GOType::GO_ENVIR:
		scene.UseEnvironment(resources.Get<neurus::Environment>(uid));
		break;
	default:
		break; // sprites / debug primitives: no add flow
	}
}

/** @brief Drops one object's scene reference by UID (typed erase; stale = no-op). */
bool RemoveSceneObject(neurus::Scene& scene, int uid)
{
	return scene.RemoveCamera(uid)
	    || scene.RemoveMesh(uid)
	    || scene.RemoveLight(uid)
	    || scene.RemoveEnvironment(uid);
}

/** @brief Adds a pooled object to the scene, selects it, and records ONE composite op. */
void OnSceneObjectAddRequested(const neurus::SceneObjectAddRequested& e,
                               const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	if (scene->GetObjectID(e.objectUid)) return; // already registered

	auto obj = ctx.resources.Get<neurus::ObjectID>(e.objectUid);
	if (!obj) return; // stale UID: resource no longer pooled

	const neurus::SelectionState before = SnapshotSelection(*scene);
	UsePooledObject(*scene, ctx.resources, e.objectUid);

	// GPU caches are scene-scoped, so an object entering the scene - live add or
	// undo/redo replay - must have its GPU resources uploaded on demand.
	const auto objType = obj->o_type;
	if (objType == neurus::ObjectID::GOType::GO_MESH
	    || objType == neurus::ObjectID::GOType::GO_LIGHT
	    || objType == neurus::ObjectID::GOType::GO_POLYLIGHT
	    || objType == neurus::ObjectID::GOType::GO_ENVIR)
	{
		ctx.events.Emit(neurus::SceneObjectGpuUploadRequested{e.objectUid});
	}

	// Select the added object (set, non-incremental).
	scene->selections.Select(obj.get(), false);

	// Light SSBO mirrors scene->light_list: rebuild after the light is in.
	neurus::notify::ObjectMutated(*obj, ctx.events);

	// One undo entry: add + select (composed selection op restores on undo).
	std::vector<std::unique_ptr<neurus::Operation>> seq;
	seq.push_back(std::make_unique<neurus::SceneObjectAddOp>(
		std::vector<int>{e.objectUid}, true));
	const neurus::SelectionState after = SnapshotSelection(*scene);
	seq.push_back(std::make_unique<neurus::SetSelectionOp>(before, after));
	ctx.ops.Submit(std::make_unique<neurus::CompositeOp>(std::move(seq)));
}

/**
 * @brief FORWARD-ONLY entry: deselect all, delete every selected object, record ONE composite op.
 *
 * This is the user-intent entry point and is never replayed; the recorded
 * composite replays through the batched removal handler. The actual removals go
 * through ONE batched request, so the removal logic lives in exactly one
 * handler, shared by the gesture and by undo/redo replay.
 */
void OnObjectDeleteRequested(const neurus::ObjectDeleteRequested&,
                             const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;

	const neurus::SelectionState before = SnapshotSelection(*scene);
	if (before.selectedUids.empty()) return;

	// Cameras are deleted like any other object - there is deliberately no
	// last-camera guard. A camera-less scene is legal: the viewport falls back to
	// its own camera, which is not scene content and cannot be deleted here.
	scene->selections.ClearSelection();

	// Emitted before the composite submit, and safe there: the removal runs
	// synchronously inside this emit, while SceneObjectAddOp below stores only a
	// UID list and a direction, so the later Submit reads no scene state the
	// removal could have invalidated.
	ctx.events.Emit(neurus::SceneObjectDeleteRequested{before.selectedUids});

	std::vector<std::unique_ptr<neurus::Operation>> seq;
	seq.push_back(std::make_unique<neurus::SetSelectionOp>(before, neurus::SelectionState{}));
	seq.push_back(std::make_unique<neurus::SceneObjectAddOp>(before.selectedUids, false));
	ctx.ops.Submit(std::make_unique<neurus::CompositeOp>(std::move(seq)));
}

/**
 * @brief The removal path (forward gesture + replay, never "replay-only").
 *
 * Removes the given scene references as a batch and emits the
 * dirty/reset/lighting events once. No selection logic and no recording: the
 * gesture records the composite, replay is muted. Stale UIDs are safe no-ops.
 */
void OnSceneObjectDeleteRequested(const neurus::SceneObjectDeleteRequested& e,
                                  const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;

	bool removedLight = false;
	bool removedAny = false;
	for (int uid : e.uids)
	{
		const neurus::ObjectID* obj = scene->GetObjectID(uid);
		if (obj && obj->IsLight())
			removedLight = true;
		if (RemoveSceneObject(*scene, uid))
			removedAny = true;
	}
	if (!removedAny) return; // all stale - safe no-op

	// The one site that branches by hand rather than through neurus::notify: a
	// delete is a batch, so the question is "was ANY of them a light", which no
	// single-object policy can answer.
	if (removedLight)
		neurus::notify::LightingRebuilt(ctx.events);
	else
		neurus::notify::Mutated(ctx.events);
}

} // anonymous namespace

namespace neurus {

void SceneController::Init(ControllerContext& ctx)
{
	// The context is captured BY VALUE so the stored handler lambdas are fully
	// self-contained.
	ctx.events.Subscribe<ObjectSelected>([ctx](const ObjectSelected& e) { OnObjectSelected(e, ctx); });
	ctx.events.Subscribe<ObjectDeselected>([ctx](const ObjectDeselected& e) { OnObjectDeselected(e, ctx); });
	ctx.events.Subscribe<SelectionChanged>([ctx](const SelectionChanged& e) { OnSelectionChanged(e, ctx); });
	ctx.events.Subscribe<ActiveCameraChanged>([ctx](const ActiveCameraChanged& e) { OnActiveCameraChanged(e, ctx); });

	// --- Scene membership (add / delete) ---
	ctx.events.Subscribe<SceneObjectAddRequested>([ctx](const SceneObjectAddRequested& e) {
		OnSceneObjectAddRequested(e, ctx);
	});
	ctx.events.Subscribe<SceneObjectDeleteRequested>([ctx](const SceneObjectDeleteRequested& e) {
		OnSceneObjectDeleteRequested(e, ctx);
	});
	ctx.events.Subscribe<ObjectDeleteRequested>([ctx](const ObjectDeleteRequested& e) {
		OnObjectDeleteRequested(e, ctx);
	});
}

} // namespace neurus
