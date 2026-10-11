/**
 * @file EditorEvents.h
 * @brief Cross-component editor events (pure data, no Qt, no Vulkan).
 *
 * Only events that involve a different component than the emitter live here:
 * render-reset, environment-changed, dirty marking and the light-GPU sync pair.
 * Scene-domain events live in SceneEvents.h; asset events in AssetEvents.h.
 */

#pragma once

namespace neurus {

/**
 * @brief Emitted when the scene state changes in a way that invalidates
 *  temporal accumulation. Subscribe to reset per-frame history.
 */
struct RenderResetEvent
{
};

/**
 * @brief Emitted when the active IBL environment is loaded or changed.
 *
 * Carries the IDs needed to locate the Environment object in the scene.
 */
struct EnvironmentChanged
{
	int sceneId = -1; ///< ID of the Scene containing the Environment
	int envId   = -1; ///< ID of the Environment object (from UID::GetObjectID())
};

/**
 * @brief Emitted after any scene mutation that constitutes a project change
 *  (transform, property, visibility, object add). Selection and camera
 *  navigation do NOT emit this.
 */
struct SceneModified
{
};

/**
 * @brief Emitted when a single light's GPU SSBO struct must be updated
 *  (power / radius / cutoff changes).
 */
struct LightGpuChanged
{
	int objectUid = 0; ///< The changed light's UID (resolved via the pool).
};

/**
 * @brief Emitted when the full light SSBO dict must be rebuilt
 *  (light transform, visibility, shadow toggle).
 */
struct LightingRebuild
{
};

/**
 * @brief Emitted when a scene object's GPU resources must exist but may not be
 *  cached yet.
 *
 * Fired when an object enters the scene (live add or undo/redo replay) and when
 * a light's shadow is toggled on. GPU caches are scene-scoped, so these need an
 * on-demand upload (skip-if-cached). Light SSBO handling stays separate; this
 * covers the per-object resources: MeshGPU, LightGPU, EnvironmentGPU.
 */
struct SceneObjectGpuUploadRequested
{
	int objectUid = 0; ///< The object entering the scene (or light with shadow just enabled).
};

} // namespace neurus
