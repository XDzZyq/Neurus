/**
 * @file SceneController.h
 * @brief Scene mutation controller -- event-driven, no per-frame polling.
 *
 * SceneController translates scene-domain events (selection, transform,
 * visibility, camera/mesh/light/environment property edits) into scene object
 * mutations. Fully stateless: all handlers are free functions in the .cpp.
 *
 * Events carry plain integer object UIDs (never raw pointers); each handler
 * resolves the UID against the current Scene (via the ControllerContext) and
 * mutates the live object. Scene-scoped state (selection, membership) is
 * reached through the context's scene provider, so a scene swap on New/Load
 * can never leave a handler holding a stale scene. Pooled-object lookups
 * (add/delete membership) go through the context's IResourceLookup.
 *
 * GPU uploads stay in Editor: this controller emits EditorEvents
 * (SceneModified, LightGpuChanged, LightingRebuild) that Editor subscribes to
 * and executes against its DeferredRenderer/UploadManager.
 *
 * Event Mapping:
 *   - ObjectSelected / ObjectDeselected -> scene.selections.Select/Deselect
 *   - VisibilityChanged -> ObjectID::SetVisible (light -> LightingRebuild)
 *   - Position/Rotation/ScaleChanged -> Transform3D setters (light -> LightingRebuild)
 *   - CameraTarget/FovChanged -> Camera setters
 *   - ActiveCameraChanged -> Scene::Activate/DeactivateCamera (0 = deactivate)
 *   - MeshShadow/MaterialChanged -> Mesh::EnableShadow/EnableMaterial
 *   - LightPower/Radius/Cutoff/OuterCutoffChanged -> setters + LightGpuChanged
 *   - LightShadowChanged -> Light::SetShadow + LightingRebuild
 *   - EnvironmentIntensity/RotationChanged -> Environment setters
 *   - Debug{Color,Opacity,XRay}Changed -> any of the three debug pools
 *   - DebugLine{Width,Stipple}Changed -> DebugLine setters
 *   - Debug{PointType,PointScale,ProjectionMode}Changed -> DebugPoints setters
 *   - DebugPositionsChanged -> DebugLine::SetVertices / DebugPoints::SetPoints
 */

#pragma once

#include "editor/controllers/Controllers.h"

namespace neurus {

class SceneController : public Controllers
{
public:
	/**
	 * @brief Constructs the controller. No provider, no stored references:
	 *        everything (event dispatch, pool lookup, operation sink, scene)
	 *        is reached through the ControllerContext passed to Init().
	 */
	SceneController() = default;

	/**
	 * @brief Subscribes to scene events on the given context.
	 *
	 * Registers lambda handlers that forward each event (with the context
	 * captured by value) to the corresponding free-function handler.
	 * Property/transform handlers capture the before value, mutate, then
	 * record a forward operation via the sink. Must be called once during
	 * initialization, before any events are enqueued.
	 *
	 * @param ctx Controller context (events, resources, ops, scene).
	 */
	void Init(ControllerContext& ctx) override;
};

} // namespace neurus
