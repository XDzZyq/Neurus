/**
 * @file SceneController.cpp
 * @brief Event-driven scene mutation handlers (selection, transforms, props).
 *
 * Every handler resolves an integer UID against the current scene and applies
 * one mutation, then routes the consequences through neurus::notify - the shared
 * mutation policy - rather than deciding them locally.
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
#include "scene/DebugLine.h"
#include "scene/DebugMesh.h"
#include "scene/DebugPoints.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "scene/Transform.h"
#include "scene/ObjectID.h"

#include "core/Log.h"

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
// Visibility
// ---------------------------------------------------------------------------

void OnVisibilityChanged(const neurus::VisibilityChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	neurus::ObjectID* obj = scene->GetObjectID(e.objectUid);
	if (!obj) return;

	const neurus::VisibilityState before{ obj->is_viewport, obj->is_rendered };
	obj->SetVisible(e.viewportVisible, e.renderVisible);
	ctx.ops.Submit(std::make_unique<neurus::SetVisibilityOp>(
		obj->GetObjectID(), before, neurus::VisibilityState{ e.viewportVisible, e.renderVisible }));

	// A light's visibility reaches the light dict rather than its entry, which
	// neurus::notify::ObjectMutated knows.
	neurus::notify::ObjectMutated(*obj, ctx.events);
}

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------

void OnPositionChanged(const neurus::PositionChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	neurus::ObjectID* obj = scene->GetObjectID(e.objectUid);
	if (!obj) return;
	void* transformPtr = obj->GetTransform();
	if (!transformPtr) return;
	auto* transform = static_cast<neurus::Transform3D*>(transformPtr);

	const glm::vec3 before = transform->GetPosition();
	const glm::vec3 after(e.posX, e.posY, e.posZ);
	transform->SetPosition(after);
	ctx.ops.Submit(std::make_unique<neurus::SetPositionOp>(obj->GetObjectID(), before, after));

	neurus::notify::ObjectMutated(*obj, ctx.events);
}

void OnRotationChanged(const neurus::RotationChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	neurus::ObjectID* obj = scene->GetObjectID(e.objectUid);
	if (!obj) return;
	void* transformPtr = obj->GetTransform();
	if (!transformPtr) return;
	auto* transform = static_cast<neurus::Transform3D*>(transformPtr);

	const glm::vec3 before = transform->GetRotation();
	const glm::vec3 after(e.rotX, e.rotY, e.rotZ);
	transform->SetRotation(after);
	ctx.ops.Submit(std::make_unique<neurus::SetRotationOp>(obj->GetObjectID(), before, after));

	neurus::notify::ObjectMutated(*obj, ctx.events);
}

void OnScaleChanged(const neurus::ScaleChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	neurus::ObjectID* obj = scene->GetObjectID(e.objectUid);
	if (!obj) return;
	void* transformPtr = obj->GetTransform();
	if (!transformPtr) return;
	auto* transform = static_cast<neurus::Transform3D*>(transformPtr);

	const glm::vec3 before = transform->GetScale();
	const glm::vec3 after(e.sclX, e.sclY, e.sclZ);
	transform->SetScale(after);
	ctx.ops.Submit(std::make_unique<neurus::SetScaleOp>(obj->GetObjectID(), before, after));

	neurus::notify::ObjectMutated(*obj, ctx.events);
}

// ---------------------------------------------------------------------------
// Camera properties
// ---------------------------------------------------------------------------

void OnCameraTargetChanged(const neurus::CameraTargetChanged& e, const neurus::ControllerContext& ctx)
{
	// Resolved through the resource pool, not the scene's camera list: the viewport
	// camera is pooled but is not scene content, so a scene-scoped lookup would drop
	// every property edit addressed to it.
	neurus::Camera* cam = ctx.resources.Get<neurus::Camera>(e.objectUid).get();
	if (!cam) return;

	const glm::vec3 pos = cam->GetPosition();
	const glm::vec3 before = cam->cam_tar;
	const glm::vec3 after(e.targetX, e.targetY, e.targetZ);
	cam->SetTarPos(after);
	// Fold target edits into the coupled camera pose op (position unchanged) so
	// the panel and viewport navigation share one undoable "Camera Transform".
	ctx.ops.Submit(std::make_unique<neurus::CameraTransformOp>(
		cam->GetObjectID(),
		neurus::CameraPose{ pos, before },
		neurus::CameraPose{ pos, after }));
	neurus::notify::Mutated(ctx.events);
}

void OnCameraFovChanged(const neurus::CameraFovChanged& e, const neurus::ControllerContext& ctx)
{
	// Pool lookup, not the scene camera list - see OnCameraTargetChanged().
	neurus::Camera* cam = ctx.resources.Get<neurus::Camera>(e.objectUid).get();
	if (!cam) return;

	const float before = cam->cam_pers;
	cam->ChangeCamPersp(e.fov);
	ctx.ops.Submit(std::make_unique<neurus::CameraFovOp>(cam->GetObjectID(), before, e.fov));
	neurus::notify::Mutated(ctx.events);
}

void OnCameraPoseChanged(const neurus::CameraPoseChanged& e, const neurus::ControllerContext& ctx)
{
	// Pool lookup, not the scene camera list - see OnCameraTargetChanged().
	neurus::Camera* cam = ctx.resources.Get<neurus::Camera>(e.objectUid).get();
	if (!cam) return;

	// Replay path for CameraTransformOp: apply the absolute pose, non-recording.
	cam->SetPosition(glm::vec3(e.posX, e.posY, e.posZ));
	cam->SetTarPos(glm::vec3(e.tarX, e.tarY, e.tarZ));
	neurus::notify::Mutated(ctx.events);
}

/**
 * @brief Activates a scene camera (or deactivates with uid 0) as ONE undo entry.
 *
 * Gesture and replay path both. The op is recorded only when the value actually
 * changes. Activating a UID that is not scene content is refused, which keeps the
 * viewport camera unactivatable. The newly activated camera needs the viewport's
 * extent, so a resize event is enqueued for it.
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
				ctx.events.enqueue(neurus::CameraResizeEvent{
					after, static_cast<int>(size.x), static_cast<int>(size.y)});
			}
		}
	}

	neurus::notify::Mutated(ctx.events);
}

// ---------------------------------------------------------------------------
// Mesh properties
// ---------------------------------------------------------------------------

void OnMeshShadowChanged(const neurus::MeshShadowChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->mesh_list.find(e.objectUid);
	if (it == scene->mesh_list.end()) return;
	neurus::Mesh* mesh = it->second.get();

	const bool before = mesh->using_shadow;
	mesh->EnableShadow(e.enabled);
	ctx.ops.Submit(std::make_unique<neurus::SetMeshShadowOp>(mesh->GetObjectID(), before, e.enabled));
	neurus::notify::Mutated(ctx.events);
}

void OnMeshMaterialChanged(const neurus::MeshMaterialChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->mesh_list.find(e.objectUid);
	if (it == scene->mesh_list.end()) return;
	neurus::Mesh* mesh = it->second.get();

	const bool before = mesh->using_material;
	mesh->EnableMaterial(e.enabled);
	ctx.ops.Submit(std::make_unique<neurus::SetMeshMaterialOp>(mesh->GetObjectID(), before, e.enabled));
	neurus::notify::Mutated(ctx.events);
}

// ---------------------------------------------------------------------------
// Light properties
// ---------------------------------------------------------------------------

void OnLightPowerChanged(const neurus::LightPowerChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->light_list.find(e.objectUid);
	if (it == scene->light_list.end()) return;
	neurus::Light* light = it->second.get();

	const float before = light->light_power;
	light->SetPower(e.power);
	ctx.ops.Submit(std::make_unique<neurus::SetLightPowerOp>(light->GetObjectID(), before, e.power));
	neurus::notify::LightStructChanged(e.objectUid, ctx.events);
}

void OnLightColorChanged(const neurus::LightColorChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->light_list.find(e.objectUid);
	if (it == scene->light_list.end()) return;
	neurus::Light* light = it->second.get();

	const glm::vec3 before = light->light_color;
	const glm::vec3 after(e.r, e.g, e.b);
	light->SetColor(after);
	ctx.ops.Submit(std::make_unique<neurus::SetLightColorOp>(light->GetObjectID(), before, after));
	neurus::notify::LightStructChanged(e.objectUid, ctx.events);
}

void OnLightRadiusChanged(const neurus::LightRadiusChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->light_list.find(e.objectUid);
	if (it == scene->light_list.end()) return;
	neurus::Light* light = it->second.get();

	const float before = light->light_radius;
	light->SetRadius(e.radius);
	ctx.ops.Submit(std::make_unique<neurus::SetLightRadiusOp>(light->GetObjectID(), before, e.radius));
	neurus::notify::LightStructChanged(e.objectUid, ctx.events);
}

void OnLightShadowChanged(const neurus::LightShadowChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->light_list.find(e.objectUid);
	if (it == scene->light_list.end()) return;
	neurus::Light* light = it->second.get();

	const bool before = light->use_shadow;
	light->SetShadow(e.enabled);
	ctx.ops.Submit(std::make_unique<neurus::SetLightShadowOp>(light->GetObjectID(), before, e.enabled));
	neurus::notify::LightingRebuilt(ctx.events);

	// Enabling shadow requires the LightGPU (shadow depth maps) to exist; it
	// was never uploaded if the light was shadowless at load/add time.
	if (e.enabled)
		ctx.events.enqueue(neurus::SceneObjectGpuUploadRequested{e.objectUid});
}

void OnLightCutoffChanged(const neurus::LightCutoffChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->light_list.find(e.objectUid);
	if (it == scene->light_list.end()) return;
	neurus::Light* light = it->second.get();

	const float before = light->spot_cutoff;
	light->SetCutoff(e.cutoff);
	ctx.ops.Submit(std::make_unique<neurus::SetLightCutoffOp>(light->GetObjectID(), before, e.cutoff));
	neurus::notify::LightStructChanged(e.objectUid, ctx.events);
}

void OnLightOuterCutoffChanged(const neurus::LightOuterCutoffChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->light_list.find(e.objectUid);
	if (it == scene->light_list.end()) return;
	neurus::Light* light = it->second.get();

	const float before = light->spot_outer_cutoff;
	light->SetOuterCutoff(e.outerCutoff);
	ctx.ops.Submit(std::make_unique<neurus::SetLightOuterCutoffOp>(light->GetObjectID(), before, e.outerCutoff));
	neurus::notify::LightStructChanged(e.objectUid, ctx.events);
}

// ---------------------------------------------------------------------------
// Environment properties
// ---------------------------------------------------------------------------

void OnEnvironmentIntensityChanged(const neurus::EnvironmentIntensityChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->env_list.find(e.objectUid);
	if (it == scene->env_list.end()) return;
	neurus::Environment* env = it->second.get();

	const float before = env->GetIntensity();
	env->SetIntensity(e.intensity);
	ctx.ops.Submit(std::make_unique<neurus::SetEnvIntensityOp>(env->GetObjectID(), before, e.intensity));
	neurus::notify::Mutated(ctx.events);
}

void OnEnvironmentRotationChanged(const neurus::EnvironmentRotationChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->env_list.find(e.objectUid);
	if (it == scene->env_list.end()) return;
	neurus::Environment* env = it->second.get();

	const float before = env->GetRotation();
	env->SetRotation(e.rotation);
	ctx.ops.Submit(std::make_unique<neurus::SetEnvRotationOp>(env->GetObjectID(), before, e.rotation));
	neurus::notify::Mutated(ctx.events);
}

// ---------------------------------------------------------------------------
// Debug object properties
// ---------------------------------------------------------------------------
//
// Editing any of these is only visible after the overlay is re-flattened, which
// is why every handler here ends in neurus::notify::Mutated().

/**
 * @brief Applies `fn` to whichever debug pool holds `uid`.
 * @return true if the UID named a live debug object.
 *
 * The three debug types share the color / opacity / x-ray knobs but no base
 * beyond ObjectID, so `fn` is a generic lambda and the three lookups stay in one
 * place. Type-specific knobs skip this and resolve their own pool directly.
 */
template <typename Fn>
bool ForDebugObject(neurus::Scene& scene, int uid, Fn&& fn)
{
	if (auto it = scene.dLine_list.find(uid); it != scene.dLine_list.end() && it->second)
	{
		fn(*it->second);
		return true;
	}
	if (auto it = scene.dPoints_list.find(uid); it != scene.dPoints_list.end() && it->second)
	{
		fn(*it->second);
		return true;
	}
	if (auto it = scene.dMesh_list.find(uid); it != scene.dMesh_list.end() && it->second)
	{
		fn(*it->second);
		return true;
	}
	return false;
}

void OnDebugColorChanged(const neurus::DebugColorChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;

	const glm::vec4 after(e.r, e.g, e.b, e.a);
	const bool found = ForDebugObject(*scene, e.objectUid, [&](auto& obj) {
		const glm::vec4 before = obj.GetColor();
		obj.SetColor(after);
		ctx.ops.Submit(std::make_unique<neurus::SetDebugColorOp>(obj.GetObjectID(), before, after));
	});
	if (found) neurus::notify::Mutated(ctx.events);
}

void OnDebugOpacityChanged(const neurus::DebugOpacityChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;

	const bool found = ForDebugObject(*scene, e.objectUid, [&](auto& obj) {
		const float before = obj.GetOpacity();
		obj.SetOpacity(e.opacity);
		ctx.ops.Submit(std::make_unique<neurus::SetDebugOpacityOp>(obj.GetObjectID(), before, e.opacity));
	});
	if (found) neurus::notify::Mutated(ctx.events);
}

void OnDebugXRayChanged(const neurus::DebugXRayChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;

	const bool found = ForDebugObject(*scene, e.objectUid, [&](auto& obj) {
		const bool before = obj.GetXRay();
		obj.SetXRay(e.xray);
		ctx.ops.Submit(std::make_unique<neurus::SetDebugXRayOp>(obj.GetObjectID(), before, e.xray));
	});
	if (found) neurus::notify::Mutated(ctx.events);
}

void OnDebugLineWidthChanged(const neurus::DebugLineWidthChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->dLine_list.find(e.objectUid);
	if (it == scene->dLine_list.end() || !it->second) return;
	neurus::DebugLine* line = it->second.get();

	const float before = line->GetWidth();
	line->SetWidth(e.width);
	ctx.ops.Submit(std::make_unique<neurus::SetDebugLineWidthOp>(line->GetObjectID(), before, e.width));
	neurus::notify::Mutated(ctx.events);
}

void OnDebugLineStippleChanged(const neurus::DebugLineStippleChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->dLine_list.find(e.objectUid);
	if (it == scene->dLine_list.end() || !it->second) return;
	neurus::DebugLine* line = it->second.get();

	const bool before = line->GetStipple();
	line->SetStipple(e.stipple);
	ctx.ops.Submit(std::make_unique<neurus::SetDebugStippleOp>(line->GetObjectID(), before, e.stipple));
	neurus::notify::Mutated(ctx.events);
}

void OnDebugPointTypeChanged(const neurus::DebugPointTypeChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->dPoints_list.find(e.objectUid);
	if (it == scene->dPoints_list.end() || !it->second) return;
	neurus::DebugPoints* points = it->second.get();

	const int before = static_cast<int>(points->GetPointType());
	points->SetPointType(static_cast<neurus::DebugPoints::PointType>(e.pointType));
	ctx.ops.Submit(std::make_unique<neurus::SetDebugPointTypeOp>(points->GetObjectID(), before, e.pointType));
	neurus::notify::Mutated(ctx.events);
}

void OnDebugPointScaleChanged(const neurus::DebugPointScaleChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->dPoints_list.find(e.objectUid);
	if (it == scene->dPoints_list.end() || !it->second) return;
	neurus::DebugPoints* points = it->second.get();

	const float before = points->GetScale();
	points->SetScale(e.scale);
	ctx.ops.Submit(std::make_unique<neurus::SetDebugPointScaleOp>(points->GetObjectID(), before, e.scale));
	neurus::notify::Mutated(ctx.events);
}

void OnDebugProjectionModeChanged(const neurus::DebugProjectionModeChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;
	auto it = scene->dPoints_list.find(e.objectUid);
	if (it == scene->dPoints_list.end() || !it->second) return;
	neurus::DebugPoints* points = it->second.get();

	const int before = points->GetProjectionMode();
	points->SetProjectionMode(e.projectionMode);
	ctx.ops.Submit(
		std::make_unique<neurus::SetDebugProjectionModeOp>(points->GetObjectID(), before, e.projectionMode));
	neurus::notify::Mutated(ctx.events);
}

/**
 * @brief Absolute position-list edit for a DebugLine or DebugPoints.
 *
 * The event carries xyz triples; a trailing partial triple is dropped rather
 * than fabricating a coordinate. DebugMesh has no editable position list, so it
 * is not reachable here.
 */
void OnDebugPositionsChanged(const neurus::DebugPositionsChanged& e, const neurus::ControllerContext& ctx)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return;

	std::vector<glm::vec3> after;
	after.reserve(e.xyz.size() / 3);
	for (size_t i = 0; i + 2 < e.xyz.size(); i += 3)
		after.emplace_back(e.xyz[i], e.xyz[i + 1], e.xyz[i + 2]);

	if (auto it = scene->dLine_list.find(e.objectUid); it != scene->dLine_list.end() && it->second)
	{
		neurus::DebugLine* line = it->second.get();
		const std::vector<glm::vec3> before = line->GetVertices();
		line->SetVertices(after);
		ctx.ops.Submit(std::make_unique<neurus::SetDebugPositionsOp>(line->GetObjectID(), before, after));
		neurus::notify::Mutated(ctx.events);
		return;
	}
	if (auto it = scene->dPoints_list.find(e.objectUid); it != scene->dPoints_list.end() && it->second)
	{
		neurus::DebugPoints* points = it->second.get();
		const std::vector<glm::vec3> before = points->GetPoints();
		points->SetPoints(after);
		ctx.ops.Submit(std::make_unique<neurus::SetDebugPositionsOp>(points->GetObjectID(), before, after));
		neurus::notify::Mutated(ctx.events);
	}
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
		ctx.events.enqueue(neurus::SceneObjectGpuUploadRequested{e.objectUid});
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
 * composite replays through the batched removal handler. The actual removals are
 * deferred as ONE batched request, so the removal logic lives in exactly one
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

	ctx.events.enqueue(neurus::SceneObjectDeleteRequested{before.selectedUids});

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
	ctx.events.subscribe<ObjectSelected>([ctx](const ObjectSelected& e) { OnObjectSelected(e, ctx); });
	ctx.events.subscribe<ObjectDeselected>([ctx](const ObjectDeselected& e) { OnObjectDeselected(e, ctx); });
	ctx.events.subscribe<SelectionChanged>([ctx](const SelectionChanged& e) { OnSelectionChanged(e, ctx); });
	ctx.events.subscribe<VisibilityChanged>([ctx](const VisibilityChanged& e) { OnVisibilityChanged(e, ctx); });
	ctx.events.subscribe<PositionChanged>([ctx](const PositionChanged& e) { OnPositionChanged(e, ctx); });
	ctx.events.subscribe<RotationChanged>([ctx](const RotationChanged& e) { OnRotationChanged(e, ctx); });
	ctx.events.subscribe<ScaleChanged>([ctx](const ScaleChanged& e) { OnScaleChanged(e, ctx); });
	ctx.events.subscribe<CameraTargetChanged>([ctx](const CameraTargetChanged& e) { OnCameraTargetChanged(e, ctx); });
	ctx.events.subscribe<CameraFovChanged>([ctx](const CameraFovChanged& e) { OnCameraFovChanged(e, ctx); });
	ctx.events.subscribe<CameraPoseChanged>([ctx](const CameraPoseChanged& e) { OnCameraPoseChanged(e, ctx); });
	ctx.events.subscribe<ActiveCameraChanged>([ctx](const ActiveCameraChanged& e) { OnActiveCameraChanged(e, ctx); });
	ctx.events.subscribe<MeshShadowChanged>([ctx](const MeshShadowChanged& e) { OnMeshShadowChanged(e, ctx); });
	ctx.events.subscribe<MeshMaterialChanged>([ctx](const MeshMaterialChanged& e) { OnMeshMaterialChanged(e, ctx); });
	ctx.events.subscribe<LightPowerChanged>([ctx](const LightPowerChanged& e) { OnLightPowerChanged(e, ctx); });
	ctx.events.subscribe<LightColorChanged>([ctx](const LightColorChanged& e) { OnLightColorChanged(e, ctx); });
	ctx.events.subscribe<LightRadiusChanged>([ctx](const LightRadiusChanged& e) { OnLightRadiusChanged(e, ctx); });
	ctx.events.subscribe<LightShadowChanged>([ctx](const LightShadowChanged& e) { OnLightShadowChanged(e, ctx); });
	ctx.events.subscribe<LightCutoffChanged>([ctx](const LightCutoffChanged& e) { OnLightCutoffChanged(e, ctx); });
	ctx.events.subscribe<LightOuterCutoffChanged>([ctx](const LightOuterCutoffChanged& e) { OnLightOuterCutoffChanged(e, ctx); });
	ctx.events.subscribe<EnvironmentIntensityChanged>([ctx](const EnvironmentIntensityChanged& e) { OnEnvironmentIntensityChanged(e, ctx); });
	ctx.events.subscribe<EnvironmentRotationChanged>([ctx](const EnvironmentRotationChanged& e) { OnEnvironmentRotationChanged(e, ctx); });
	ctx.events.subscribe<DebugColorChanged>([ctx](const DebugColorChanged& e) { OnDebugColorChanged(e, ctx); });
	ctx.events.subscribe<DebugOpacityChanged>([ctx](const DebugOpacityChanged& e) { OnDebugOpacityChanged(e, ctx); });
	ctx.events.subscribe<DebugXRayChanged>([ctx](const DebugXRayChanged& e) { OnDebugXRayChanged(e, ctx); });
	ctx.events.subscribe<DebugLineWidthChanged>([ctx](const DebugLineWidthChanged& e) { OnDebugLineWidthChanged(e, ctx); });
	ctx.events.subscribe<DebugLineStippleChanged>([ctx](const DebugLineStippleChanged& e) { OnDebugLineStippleChanged(e, ctx); });
	ctx.events.subscribe<DebugPointTypeChanged>([ctx](const DebugPointTypeChanged& e) { OnDebugPointTypeChanged(e, ctx); });
	ctx.events.subscribe<DebugPointScaleChanged>([ctx](const DebugPointScaleChanged& e) { OnDebugPointScaleChanged(e, ctx); });
	ctx.events.subscribe<DebugProjectionModeChanged>([ctx](const DebugProjectionModeChanged& e) { OnDebugProjectionModeChanged(e, ctx); });
	ctx.events.subscribe<DebugPositionsChanged>([ctx](const DebugPositionsChanged& e) { OnDebugPositionsChanged(e, ctx); });

	// --- Scene membership (add / delete) ---
	ctx.events.subscribe<SceneObjectAddRequested>([ctx](const SceneObjectAddRequested& e) {
		OnSceneObjectAddRequested(e, ctx);
	});
	ctx.events.subscribe<SceneObjectDeleteRequested>([ctx](const SceneObjectDeleteRequested& e) {
		OnSceneObjectDeleteRequested(e, ctx);
	});
	ctx.events.subscribe<ObjectDeleteRequested>([ctx](const ObjectDeleteRequested& e) {
		OnObjectDeleteRequested(e, ctx);
	});
}

} // namespace neurus
