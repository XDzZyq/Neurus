/**
 * @file ObjectController.cpp
 * @brief Event-driven pooled-object property handlers.
 *
 * Every handler resolves the event's integer UID against the resource pool and
 * applies one mutation, then routes the consequences through neurus::notify -
 * the shared mutation policy - rather than deciding them locally.
 */

#include "editor/controllers/ObjectController.h"

#include <vector>

#include "editor/controllers/MutationNotify.h"
#include "editor/events/EditorEvents.h"
#include "editor/events/SceneEvents.h"
#include "editor/operations/SceneOperations.h"

#include "scene/Camera.h"
#include "scene/DebugLine.h"
#include "scene/DebugMesh.h"
#include "scene/DebugPoints.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/ObjectID.h"
#include "scene/Transform.h"

namespace {

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

void OnVisibilityChanged(const neurus::VisibilityChanged& e, const neurus::ControllerContext& ctx)
{
	auto obj = ctx.resources.Get<neurus::ObjectID>(e.objectUid);
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
	auto obj = ctx.resources.Get<neurus::ObjectID>(e.objectUid);
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
	auto obj = ctx.resources.Get<neurus::ObjectID>(e.objectUid);
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
	auto obj = ctx.resources.Get<neurus::ObjectID>(e.objectUid);
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
	auto cam = ctx.resources.Get<neurus::Camera>(e.objectUid);
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
	auto cam = ctx.resources.Get<neurus::Camera>(e.objectUid);
	if (!cam) return;

	const float before = cam->cam_pers;
	cam->ChangeCamPersp(e.fov);
	ctx.ops.Submit(std::make_unique<neurus::CameraFovOp>(cam->GetObjectID(), before, e.fov));
	neurus::notify::Mutated(ctx.events);
}

void OnCameraPoseChanged(const neurus::CameraPoseChanged& e, const neurus::ControllerContext& ctx)
{
	// Pool lookup, not the scene camera list - see OnCameraTargetChanged().
	auto cam = ctx.resources.Get<neurus::Camera>(e.objectUid);
	if (!cam) return;

	// Replay path for CameraTransformOp: apply the absolute pose, non-recording.
	cam->SetPosition(glm::vec3(e.posX, e.posY, e.posZ));
	cam->SetTarPos(glm::vec3(e.tarX, e.tarY, e.tarZ));
	neurus::notify::Mutated(ctx.events);
}

// ---------------------------------------------------------------------------
// Mesh properties
// ---------------------------------------------------------------------------

void OnMeshShadowChanged(const neurus::MeshShadowChanged& e, const neurus::ControllerContext& ctx)
{
	auto mesh = ctx.resources.Get<neurus::Mesh>(e.objectUid);
	if (!mesh) return;

	const bool before = mesh->using_shadow;
	mesh->EnableShadow(e.enabled);
	ctx.ops.Submit(std::make_unique<neurus::SetMeshShadowOp>(mesh->GetObjectID(), before, e.enabled));
	neurus::notify::Mutated(ctx.events);
}

void OnMeshMaterialChanged(const neurus::MeshMaterialChanged& e, const neurus::ControllerContext& ctx)
{
	auto mesh = ctx.resources.Get<neurus::Mesh>(e.objectUid);
	if (!mesh) return;

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
	auto light = ctx.resources.Get<neurus::Light>(e.objectUid);
	if (!light) return;

	const float before = light->light_power;
	light->SetPower(e.power);
	ctx.ops.Submit(std::make_unique<neurus::SetLightPowerOp>(light->GetObjectID(), before, e.power));
	neurus::notify::LightStructChanged(e.objectUid, ctx.events);
}

void OnLightColorChanged(const neurus::LightColorChanged& e, const neurus::ControllerContext& ctx)
{
	auto light = ctx.resources.Get<neurus::Light>(e.objectUid);
	if (!light) return;

	const glm::vec3 before = light->light_color;
	const glm::vec3 after(e.r, e.g, e.b);
	light->SetColor(after);
	ctx.ops.Submit(std::make_unique<neurus::SetLightColorOp>(light->GetObjectID(), before, after));
	neurus::notify::LightStructChanged(e.objectUid, ctx.events);
}

void OnLightRadiusChanged(const neurus::LightRadiusChanged& e, const neurus::ControllerContext& ctx)
{
	auto light = ctx.resources.Get<neurus::Light>(e.objectUid);
	if (!light) return;

	const float before = light->light_radius;
	light->SetRadius(e.radius);
	ctx.ops.Submit(std::make_unique<neurus::SetLightRadiusOp>(light->GetObjectID(), before, e.radius));
	neurus::notify::LightStructChanged(e.objectUid, ctx.events);
}

void OnLightShadowChanged(const neurus::LightShadowChanged& e, const neurus::ControllerContext& ctx)
{
	auto light = ctx.resources.Get<neurus::Light>(e.objectUid);
	if (!light) return;

	const bool before = light->use_shadow;
	light->SetShadow(e.enabled);
	ctx.ops.Submit(std::make_unique<neurus::SetLightShadowOp>(light->GetObjectID(), before, e.enabled));
	neurus::notify::LightingRebuilt(ctx.events);

	// Enabling shadow requires the LightGPU (shadow depth maps) to exist; it
	// was never uploaded if the light was shadowless at load/add time.
	if (e.enabled)
		ctx.events.Emit(neurus::SceneObjectGpuUploadRequested{e.objectUid});
}

void OnLightCutoffChanged(const neurus::LightCutoffChanged& e, const neurus::ControllerContext& ctx)
{
	auto light = ctx.resources.Get<neurus::Light>(e.objectUid);
	if (!light) return;

	const float before = light->spot_cutoff;
	light->SetCutoff(e.cutoff);
	ctx.ops.Submit(std::make_unique<neurus::SetLightCutoffOp>(light->GetObjectID(), before, e.cutoff));
	neurus::notify::LightStructChanged(e.objectUid, ctx.events);
}

void OnLightOuterCutoffChanged(const neurus::LightOuterCutoffChanged& e, const neurus::ControllerContext& ctx)
{
	auto light = ctx.resources.Get<neurus::Light>(e.objectUid);
	if (!light) return;

	const float before = light->spot_outer_cutoff;
	light->SetOuterCutoff(e.outerCutoff);
	ctx.ops.Submit(std::make_unique<neurus::SetLightOuterCutoffOp>(light->GetObjectID(), before, e.outerCutoff));
	neurus::notify::LightStructChanged(e.objectUid, ctx.events);
}

// ---------------------------------------------------------------------------
// Environment properties
// ---------------------------------------------------------------------------

void OnEnvironmentIntensityChanged(const neurus::EnvironmentIntensityChanged& e,
                                   const neurus::ControllerContext& ctx)
{
	auto env = ctx.resources.Get<neurus::Environment>(e.objectUid);
	if (!env) return;

	const float before = env->GetIntensity();
	env->SetIntensity(e.intensity);
	ctx.ops.Submit(std::make_unique<neurus::SetEnvIntensityOp>(env->GetObjectID(), before, e.intensity));
	neurus::notify::Mutated(ctx.events);
}

void OnEnvironmentRotationChanged(const neurus::EnvironmentRotationChanged& e,
                                  const neurus::ControllerContext& ctx)
{
	auto env = ctx.resources.Get<neurus::Environment>(e.objectUid);
	if (!env) return;

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
 * @brief Applies `fn` to whichever pooled debug object `uid` names.
 * @return true if the UID named a pooled debug object.
 *
 * The three debug types all expose color / opacity / x-ray (they share ObjectID
 * and Transform3D), but no shared base declares those knobs, so `fn` is a generic
 * lambda and the three pool lookups stay in one place. Type-specific knobs skip
 * this and resolve their own type directly.
 */
template <typename Fn>
bool ForDebugObject(const neurus::IResourceLookup& resources, int uid, Fn&& fn)
{
	if (auto line = resources.Get<neurus::DebugLine>(uid))
	{
		fn(*line);
		return true;
	}
	if (auto points = resources.Get<neurus::DebugPoints>(uid))
	{
		fn(*points);
		return true;
	}
	if (auto mesh = resources.Get<neurus::DebugMesh>(uid))
	{
		fn(*mesh);
		return true;
	}
	return false;
}

void OnDebugColorChanged(const neurus::DebugColorChanged& e, const neurus::ControllerContext& ctx)
{
	const glm::vec4 after(e.r, e.g, e.b, e.a);
	const bool found = ForDebugObject(ctx.resources, e.objectUid, [&](auto& obj) {
		const glm::vec4 before = obj.GetColor();
		obj.SetColor(after);
		ctx.ops.Submit(std::make_unique<neurus::SetDebugColorOp>(obj.GetObjectID(), before, after));
	});
	if (found) neurus::notify::Mutated(ctx.events);
}

void OnDebugOpacityChanged(const neurus::DebugOpacityChanged& e, const neurus::ControllerContext& ctx)
{
	const bool found = ForDebugObject(ctx.resources, e.objectUid, [&](auto& obj) {
		const float before = obj.GetOpacity();
		obj.SetOpacity(e.opacity);
		ctx.ops.Submit(std::make_unique<neurus::SetDebugOpacityOp>(obj.GetObjectID(), before, e.opacity));
	});
	if (found) neurus::notify::Mutated(ctx.events);
}

void OnDebugXRayChanged(const neurus::DebugXRayChanged& e, const neurus::ControllerContext& ctx)
{
	const bool found = ForDebugObject(ctx.resources, e.objectUid, [&](auto& obj) {
		const bool before = obj.GetXRay();
		obj.SetXRay(e.xray);
		ctx.ops.Submit(std::make_unique<neurus::SetDebugXRayOp>(obj.GetObjectID(), before, e.xray));
	});
	if (found) neurus::notify::Mutated(ctx.events);
}

void OnDebugLineWidthChanged(const neurus::DebugLineWidthChanged& e, const neurus::ControllerContext& ctx)
{
	auto line = ctx.resources.Get<neurus::DebugLine>(e.objectUid);
	if (!line) return;

	const float before = line->GetWidth();
	line->SetWidth(e.width);
	ctx.ops.Submit(std::make_unique<neurus::SetDebugLineWidthOp>(line->GetObjectID(), before, e.width));
	neurus::notify::Mutated(ctx.events);
}

void OnDebugLineStippleChanged(const neurus::DebugLineStippleChanged& e, const neurus::ControllerContext& ctx)
{
	auto line = ctx.resources.Get<neurus::DebugLine>(e.objectUid);
	if (!line) return;

	const bool before = line->GetStipple();
	line->SetStipple(e.stipple);
	ctx.ops.Submit(std::make_unique<neurus::SetDebugStippleOp>(line->GetObjectID(), before, e.stipple));
	neurus::notify::Mutated(ctx.events);
}

void OnDebugPointTypeChanged(const neurus::DebugPointTypeChanged& e, const neurus::ControllerContext& ctx)
{
	auto points = ctx.resources.Get<neurus::DebugPoints>(e.objectUid);
	if (!points) return;

	const int before = static_cast<int>(points->GetPointType());
	points->SetPointType(static_cast<neurus::DebugPoints::PointType>(e.pointType));
	ctx.ops.Submit(std::make_unique<neurus::SetDebugPointTypeOp>(points->GetObjectID(), before, e.pointType));
	neurus::notify::Mutated(ctx.events);
}

void OnDebugPointScaleChanged(const neurus::DebugPointScaleChanged& e, const neurus::ControllerContext& ctx)
{
	auto points = ctx.resources.Get<neurus::DebugPoints>(e.objectUid);
	if (!points) return;

	const float before = points->GetScale();
	points->SetScale(e.scale);
	ctx.ops.Submit(std::make_unique<neurus::SetDebugPointScaleOp>(points->GetObjectID(), before, e.scale));
	neurus::notify::Mutated(ctx.events);
}

void OnDebugProjectionModeChanged(const neurus::DebugProjectionModeChanged& e,
                                  const neurus::ControllerContext& ctx)
{
	auto points = ctx.resources.Get<neurus::DebugPoints>(e.objectUid);
	if (!points) return;

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
	std::vector<glm::vec3> after;
	after.reserve(e.xyz.size() / 3);
	for (size_t i = 0; i + 2 < e.xyz.size(); i += 3)
		after.emplace_back(e.xyz[i], e.xyz[i + 1], e.xyz[i + 2]);

	if (auto line = ctx.resources.Get<neurus::DebugLine>(e.objectUid))
	{
		const std::vector<glm::vec3> before = line->GetVertices();
		line->SetVertices(after);
		ctx.ops.Submit(std::make_unique<neurus::SetDebugPositionsOp>(line->GetObjectID(), before, after));
		neurus::notify::Mutated(ctx.events);
		return;
	}
	if (auto points = ctx.resources.Get<neurus::DebugPoints>(e.objectUid))
	{
		const std::vector<glm::vec3> before = points->GetPoints();
		points->SetPoints(after);
		ctx.ops.Submit(std::make_unique<neurus::SetDebugPositionsOp>(points->GetObjectID(), before, after));
		neurus::notify::Mutated(ctx.events);
	}
}

} // anonymous namespace

namespace neurus {

void ObjectController::Init(ControllerContext& ctx)
{
	// The context is captured BY VALUE so the stored handler lambdas are fully
	// self-contained.
	ctx.events.Subscribe<VisibilityChanged>([ctx](const VisibilityChanged& e) { OnVisibilityChanged(e, ctx); });
	ctx.events.Subscribe<PositionChanged>([ctx](const PositionChanged& e) { OnPositionChanged(e, ctx); });
	ctx.events.Subscribe<RotationChanged>([ctx](const RotationChanged& e) { OnRotationChanged(e, ctx); });
	ctx.events.Subscribe<ScaleChanged>([ctx](const ScaleChanged& e) { OnScaleChanged(e, ctx); });
	ctx.events.Subscribe<CameraTargetChanged>([ctx](const CameraTargetChanged& e) { OnCameraTargetChanged(e, ctx); });
	ctx.events.Subscribe<CameraFovChanged>([ctx](const CameraFovChanged& e) { OnCameraFovChanged(e, ctx); });
	ctx.events.Subscribe<CameraPoseChanged>([ctx](const CameraPoseChanged& e) { OnCameraPoseChanged(e, ctx); });
	ctx.events.Subscribe<MeshShadowChanged>([ctx](const MeshShadowChanged& e) { OnMeshShadowChanged(e, ctx); });
	ctx.events.Subscribe<MeshMaterialChanged>([ctx](const MeshMaterialChanged& e) { OnMeshMaterialChanged(e, ctx); });
	ctx.events.Subscribe<LightPowerChanged>([ctx](const LightPowerChanged& e) { OnLightPowerChanged(e, ctx); });
	ctx.events.Subscribe<LightColorChanged>([ctx](const LightColorChanged& e) { OnLightColorChanged(e, ctx); });
	ctx.events.Subscribe<LightRadiusChanged>([ctx](const LightRadiusChanged& e) { OnLightRadiusChanged(e, ctx); });
	ctx.events.Subscribe<LightShadowChanged>([ctx](const LightShadowChanged& e) { OnLightShadowChanged(e, ctx); });
	ctx.events.Subscribe<LightCutoffChanged>([ctx](const LightCutoffChanged& e) { OnLightCutoffChanged(e, ctx); });
	ctx.events.Subscribe<LightOuterCutoffChanged>(
		[ctx](const LightOuterCutoffChanged& e) { OnLightOuterCutoffChanged(e, ctx); });
	ctx.events.Subscribe<EnvironmentIntensityChanged>(
		[ctx](const EnvironmentIntensityChanged& e) { OnEnvironmentIntensityChanged(e, ctx); });
	ctx.events.Subscribe<EnvironmentRotationChanged>(
		[ctx](const EnvironmentRotationChanged& e) { OnEnvironmentRotationChanged(e, ctx); });
	ctx.events.Subscribe<DebugColorChanged>([ctx](const DebugColorChanged& e) { OnDebugColorChanged(e, ctx); });
	ctx.events.Subscribe<DebugOpacityChanged>([ctx](const DebugOpacityChanged& e) { OnDebugOpacityChanged(e, ctx); });
	ctx.events.Subscribe<DebugXRayChanged>([ctx](const DebugXRayChanged& e) { OnDebugXRayChanged(e, ctx); });
	ctx.events.Subscribe<DebugLineWidthChanged>(
		[ctx](const DebugLineWidthChanged& e) { OnDebugLineWidthChanged(e, ctx); });
	ctx.events.Subscribe<DebugLineStippleChanged>(
		[ctx](const DebugLineStippleChanged& e) { OnDebugLineStippleChanged(e, ctx); });
	ctx.events.Subscribe<DebugPointTypeChanged>(
		[ctx](const DebugPointTypeChanged& e) { OnDebugPointTypeChanged(e, ctx); });
	ctx.events.Subscribe<DebugPointScaleChanged>(
		[ctx](const DebugPointScaleChanged& e) { OnDebugPointScaleChanged(e, ctx); });
	ctx.events.Subscribe<DebugProjectionModeChanged>(
		[ctx](const DebugProjectionModeChanged& e) { OnDebugProjectionModeChanged(e, ctx); });
	ctx.events.Subscribe<DebugPositionsChanged>(
		[ctx](const DebugPositionsChanged& e) { OnDebugPositionsChanged(e, ctx); });
}

} // namespace neurus
