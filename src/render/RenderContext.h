/**
 * @file RenderContext.h
 * @brief Per-frame context data passed to all render passes.
 *
 * RenderContext aggregates the per-frame state that multiple passes (Geometry,
 * Lighting, SSAO, IBL) need to record their commands: render extent for
 * viewport/scissor, frame index for descriptor ring-buffer slots, camera
 * matrices for shading, and optional scene pointers for draw batches and
 * light data.
 *
 * Architecture:
 * - Pure data struct with no methods — owned and populated by the caller
 *   (DeferredRenderer or test fixture) each frame.
 * - Passes iterate snapshot.scene->mesh_list for meshes and ->light_list for lights.
 * - The frame's scene view is embedded **by value** as `snapshot` and declared
 *   in exactly one place (scene/SceneContext.h), shared with UIContext. This
 *   struct never re-declares `scene`, `config`, `camera`, `debugDraw` or
 *   `gizmoDraw`, and nothing maps them field-by-field.
 * - `snapshot` answers for its own members' nullability — `HasScene()`,
 *   `HasCamera()`, `Settings()` — so a pass that needs nothing from the scene
 *   (a standalone test) still works with a default-constructed snapshot.
 *
 * @note This is NOT a GPU push-constant block — it's CPU-side metadata
 *       carried through the pass execution chain.
 */

#pragma once

#include <cstdint>

#include <glm/glm.hpp>

#include "scene/SceneContext.h"

namespace neurus
{

/**
 * @brief Per-frame context for all render passes.
 *
 * Populated once per frame by the renderer and passed (typically as const&)
 * to each pass's RecordCommandBuffer / Draw / Dispatch call.
 */
struct RenderContext
{
	/// @brief Render area width in pixels (used for viewport, scissor, dispatch groups).
	uint32_t width = 0;

	/// @brief Render area height in pixels (used for viewport, scissor, dispatch groups).
	uint32_t height = 0;

	/// @brief Ring-buffer slot index for per-frame descriptor pools / UBOs.
	uint32_t frameIndex = 0;

	/// @brief The frame's scene view, shared by value with UIContext.
	///
	/// Every member is a typed pointer (see scene/SceneContext.h), so a pass
	/// reads `snapshot.scene` / `snapshot.camera` with no cast, and reaches the
	/// render settings through `snapshot.Settings()`, which is never null and
	/// resolves to RenderConfig's own declared defaults. The null policy for the
	/// other members lives at that header, stated once, so no pass re-decides it.
	SceneContext snapshot;

	/// @brief Per-frame random 3D direction for shadow jitter (normalized unit-ball vector).
	/// Point lights use: pos_jittered = pos + light.radius * jitter
	/// Sun lights use: dir_jittered = normalize(lightDir + jitter * smallScale)
	glm::vec3 jitter{0.0f, 0.0f, 0.0f};

	/// @brief Per-frame iteration counter (incremented each frame, reset by Editor on scene changes).
	uint32_t iteration{0};

};

} // namespace neurus
