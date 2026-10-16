/**
 * @file RenderContext.h
 * @brief Per-frame context data passed to all render passes.
 *
 * Aggregates the per-frame state a pass needs to record its commands: render
 * extent, frame index, camera matrices, the scene, and the overlay payloads.
 *
 * Two writers, one struct, each staying out of the other's half: the Editor
 * publishes what is being rendered (scene, config, camera, debugDraw,
 * gizmoDraw), the renderer stamps which frame (width, height, frameIndex,
 * jitter, iteration). The `---` banners below mark each field's half, and the
 * renderer half is stamped into a local copy inside recordFrame, so the
 * temporal counters stay renderer-owned.
 *
 * Vulkan-free, by construction: one-way state from the Editor to the render
 * layer, carrying no Vulkan type.
 *
 * The Editor half is in transition: `snapshot` carries the same frame as an
 * owned deep copy (RenderFrameSnapshot), which is what a render thread can read
 * safely. Both halves are published while the passes still read the pointers.
 *
 * @note This is NOT a GPU push-constant block - it's CPU-side metadata
 *       carried through the pass execution chain.
 */

#pragma once

#include <cstdint>

#include <glm/glm.hpp>

#include "render/overlay/DebugDrawList.h"
#include "render/overlay/GizmoDrawList.h"

namespace neurus
{

class Camera;
class RenderConfig;
class Scene;
struct RenderFrameSnapshot;

/**
 * @brief Per-frame context for all render passes.
 *
 * Populated once per frame by the Editor (semantic half) and the renderer
 * (frame half), then passed (typically as const&) to each pass's
 * RecordCommandBuffer / Draw / Dispatch call.
 */
struct RenderContext
{
	// -----------------------------------------------------------------------
	// Editor half — what is being rendered. Filled by Editor::UpdateRenderContext().
	// -----------------------------------------------------------------------

	/// @brief The frame's scene, or nullptr. Check HasScene() before dereferencing.
	const Scene* scene = nullptr;

	/**
	 * @brief The Editor-owned render settings, or nullptr.
	 *
	 * Read it through Settings(), never directly: that keeps RenderConfig's
	 * in-class member initializers the only place a default is written.
	 */
	const RenderConfig* config = nullptr;

	/**
	 * @brief The camera this frame is being viewed through, or nullptr.
	 *
	 * The Editor publishes the frame's single viewing camera, and every consumer
	 * reads this same pointer, so they cannot disagree. Two cameras disagreeing
	 * here does not fail - it renders plausible-looking wrong lighting.
	 */
	const Camera* camera = nullptr;

	/**
	 * @brief This frame's debug geometry, or nullptr when debug draw is off.
	 *
	 * Rebuilt on a dirty flag rather than per frame, and valid only for the frame
	 * it was published in. Null is a legal frame (debug draw off, or no scene
	 * yet), so consumers check before touching it.
	 */
	const DebugDrawList* debugDraw = nullptr;

	/**
	 * @brief This frame's transform-gizmo geometry; empty when nothing is selected.
	 *
	 * Published unconditionally - interaction feedback must not be hideable by a
	 * debug-visualization toggle - and published empty rather than null, so the
	 * pass early-outs on null and on empty alike.
	 */
	const GizmoDrawList* gizmoDraw = nullptr;

	/**
	 * @brief This frame as an owned deep copy, or nullptr.
	 *
	 * The borrowed half above is only safe while the Editor and the renderer
	 * share a thread; this is the same frame with the borrowing taken out (see
	 * RenderFrameSnapshot.h). It is published alongside the pointers rather than
	 * instead of them: the passes still read `scene` / `camera` / `debugDraw`,
	 * and moving them over - which is what retires the borrowed half - is a
	 * separate step from making the copy exist.
	 *
	 * Null is a legal frame, and is what every frame carries until a producer
	 * owns storage that outlives the frame. Check HasSnapshot() first.
	 */
	const RenderFrameSnapshot* snapshot = nullptr;

	// -----------------------------------------------------------------------
	// Renderer half — which frame. Stamped by DeferredRenderer::recordFrame().
	// -----------------------------------------------------------------------

	/// @brief Render area width in pixels (used for viewport, scissor, dispatch groups).
	uint32_t width = 0;

	/// @brief Render area height in pixels (used for viewport, scissor, dispatch groups).
	uint32_t height = 0;

	/// @brief Ring-buffer slot index for per-frame descriptor pools / UBOs.
	uint32_t frameIndex = 0;

	/// @brief Per-frame random unit-ball direction used to jitter shadow sampling.
	glm::vec3 jitter{0.0f, 0.0f, 0.0f};

	/// @brief Per-frame iteration counter (incremented each frame; reset on scene changes).
	uint32_t iteration{0};

	// --- Null policy for the Editor half (see the file header) ---

	/** @brief True when a Scene was published; required before touching `scene`. */
	bool HasScene() const { return scene != nullptr; }

	/** @brief True when a viewing camera was published. */
	bool HasCamera() const { return camera != nullptr; }

	/** @brief True when an owned frame snapshot was published. */
	bool HasSnapshot() const { return snapshot != nullptr; }

	/**
	 * @brief This frame as owned data - never null, so no reader checks.
	 *
	 * The same null policy Settings() follows: an unpublished frame reads as an
	 * empty one (no scene, no camera, no draws, default settings) rather than as
	 * a pointer every pass has to test. A pass that skipped on a null Scene now
	 * skips on `Frame().HasScene()` and needs no second branch.
	 *
	 * This is the field passes read; the borrowed pointers above are what it
	 * replaces.
	 *
	 * Defined in the .cpp, where the complete RenderFrameSnapshot is available.
	 */
	const RenderFrameSnapshot& Frame() const;

	/**
	 * @brief This frame's render settings, or the built-in defaults.
	 *
	 * Never null, so a reader needs no fallback literal of its own: the fallback
	 * is DefaultRenderConfig(), which is RenderConfig's member initializers
	 * rather than a second copy, so a default cannot drift from the declaration.
	 *
	 * Defined in the .cpp, where the complete RenderConfig type is available.
	 */
	const RenderConfig& Settings() const;
};

} // namespace neurus
