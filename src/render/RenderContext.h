/**
 * @file RenderContext.h
 * @brief Per-frame context data passed to all render passes.
 *
 * Aggregates the per-frame state a pass needs to record its commands: the owned
 * frame snapshot the Editor published, plus the render extent, frame index and
 * temporal counters the renderer stamps.
 *
 * Two writers, one struct, each staying out of the other's half: the Editor
 * publishes what is being rendered as one owned snapshot, the renderer stamps
 * which frame (width, height, frameIndex, jitter, iteration). The `---` banners
 * below mark each field's half, and the renderer half is stamped into a local
 * copy inside recordFrame, so the temporal counters stay renderer-owned.
 *
 * Vulkan-free, by construction: one-way state from the Editor to the render
 * layer, carrying no Vulkan type.
 *
 * @note This is NOT a GPU push-constant block - it's CPU-side metadata
 *       carried through the pass execution chain.
 */

#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace neurus
{

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
	// Editor half — what is being rendered. Filled by Editor::BuildSnapshot().
	// -----------------------------------------------------------------------

	/**
	 * @brief This frame as an owned deep copy, or nullptr.
	 *
	 * The whole frame the Editor published, with no borrowing left in it (see
	 * RenderFrameSnapshot.h): every value is copied out of the Scene while the
	 * Editor still owns it, so a reader may keep it for as long as it likes.
	 *
	 * Null is a legal frame, and is what every frame carries until a producer
	 * owns storage that outlives the frame. Read it through Frame(), which turns
	 * an unpublished context into the empty frame, so no pass tests the pointer.
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

	/** @brief True when an owned frame snapshot was published. */
	bool HasSnapshot() const { return snapshot != nullptr; }

	/**
	 * @brief This frame as owned data - never null, so no reader checks.
	 *
	 * An unpublished frame reads as an empty one (no scene, no camera, no draws,
	 * default settings) rather than as a pointer every pass has to test. A pass
	 * that skipped on a null Scene now skips on `Frame().HasScene()` and needs no
	 * second branch.
	 *
	 * This is the one field passes read for what is being rendered.
	 *
	 * Defined in the .cpp, where the complete RenderFrameSnapshot is available.
	 */
	const RenderFrameSnapshot& Frame() const;
};

} // namespace neurus
