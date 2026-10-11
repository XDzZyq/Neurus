/**
 * @file RenderContext.h
 * @brief Per-frame context data passed to all render passes.
 *
 * Aggregates the per-frame state that passes need to record their commands:
 * render extent for viewport/scissor, frame index for descriptor ring-buffer
 * slots, camera matrices for shading, and the optional scene view.
 *
 * @note This is NOT a GPU push-constant block - it's CPU-side metadata
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

	/// @brief The frame's scene view (see scene/SceneContext.h).
	///
	/// Every member is a typed pointer, so a pass reads `snapshot.scene` /
	/// `snapshot.camera` with no cast and reaches the render settings through
	/// `snapshot.Settings()`, which is never null.
	SceneContext snapshot;

	/// @brief Per-frame random 3D direction for shadow jitter (normalized unit-ball vector).
	glm::vec3 jitter{0.0f, 0.0f, 0.0f};

	/// @brief Per-frame iteration counter (incremented each frame; the caller
	///        resets it when temporal history is invalidated).
	uint32_t iteration{0};

};

} // namespace neurus
