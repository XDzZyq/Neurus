/**
 * @file RenderContext.h
 * @brief Per-frame context data passed to all render passes.
 *
 * RenderContext aggregates the per-frame state that multiple passes (Geometry,
 * Lighting, SSAO, IBL) need to record their commands: render extent for
 * viewport/scissor, frame index for descriptor ring-buffer slots, camera
 * matrices for shading, the scene view for draw batches and light data, and the
 * overlay payloads the debug and gizmo passes consume.
 *
 * Architecture:
 * - Pure data struct plus the two query helpers it answers for itself. Owned and
 *   populated by the caller (DeferredRenderer or a test fixture) each frame.
 * - **Two writers, one struct, and each stays out of the other's half.** The
 *   Editor publishes what is being rendered; the renderer stamps when:
 *
 *       Editor::UpdateRenderContext()   scene, config, camera, debugDraw, gizmoDraw
 *       DeferredRenderer::recordFrame() width, height, frameIndex, jitter, iteration
 *
 *   Every field carries a `---` banner saying which of the two fills it. A new
 *   field belongs to whichever half owns its *source*: if it comes from Editor
 *   state (the Scene, the RenderConfig, the viewport) it is the Editor's; if it
 *   comes from the swapchain, the frame slot ring or a per-frame counter it is
 *   the renderer's. Width/height are the case that looks like it could go either
 *   way and cannot: EditorViewport knows the surface size, but the extent a pass
 *   must use is the *acquired image's* (DeferredRenderer::recordFrame), and the
 *   two disagree during a resize.
 * - The renderer half is stamped into a **local copy** inside recordFrame, which
 *   is what keeps `m_iteration` / `m_haltonIndex` renderer-owned: the caller's
 *   context never carries temporal state, and the Editor can neither read nor
 *   reset it. Keep that copy when editing recordFrame.
 * - **Vulkan-free, by construction.** Like UIContext is Qt-free: this is one-way
 *   state from the Editor to the render layer, it carries no Vulkan type, and its
 *   only project dependencies are Vulkan-free types (Scene, Camera, RenderConfig,
 *   the overlay payloads). A member that needed a `vk::` type would mean the
 *   context had started describing the GPU instead of the frame.
 * - The context answers for its own members' nullability — HasScene(),
 *   HasCamera(), Settings() — so a pass that needs nothing from the scene (a
 *   standalone test) still works with a default-constructed `RenderContext`.
 *
 * @note This is NOT a GPU push-constant block — it's CPU-side metadata
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

/**
 * @brief Per-frame context for all render passes.
 *
 * Populated once per frame by the Editor (semantic half) and the renderer
 * (frame half) and passed (typically as const&) to each pass's
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
	 * Read it through Settings(), never directly: that is what keeps
	 * RenderConfig's in-class member initializers the only place a default is
	 * written. Reading this pointer with a `cfg ? cfg->x : <literal>` ternary is
	 * how the shadow-bias default came to differ 40x between the pass and the
	 * declaration.
	 */
	const RenderConfig* config = nullptr;

	/**
	 * @brief The camera this frame is being viewed through, or nullptr.
	 *
	 * Published from EditorViewport::GetCamera(), which is deliberately the only
	 * Editor-side definition of the viewing camera: the gizmo's projection math, the
	 * renderer's CameraGPU and every pass that builds a view-projection or centres a
	 * shadow frustum read the same pointer, so they cannot disagree. Two cameras
	 * disagreeing here does not fail - it renders plausible-looking wrong lighting.
	 * Today it is the Scene's active camera; when the viewport owns a free camera
	 * it will not be, and nothing downstream needs to change.
	 *
	 * A test driving a pass directly stands in for the frame driver:
	 * VulkanTestShared::PublishSceneCamera(cache, ctx) fills this and the
	 * CameraGPU together.
	 */
	const Camera* camera = nullptr;

	/**
	 * @brief This frame's debug geometry, or nullptr when debug draw is off.
	 *
	 * Owned by the Editor and rebuilt on a dirty flag, not per frame; valid only
	 * for the frame it was published in. Null is a legal frame (r_debug_draw off,
	 * or no scene yet), so DebugPass checks before touching it.
	 */
	const DebugDrawList* debugDraw = nullptr;

	/**
	 * @brief This frame's transform-gizmo geometry; empty when nothing is selected.
	 *
	 * Carries the resting translate handle on the selection's active object, or the
	 * armed gesture's guide while one is live. Published unconditionally — there is
	 * deliberately no RenderConfig flag gating it, because interaction feedback must
	 * not be hideable by a debug-visualization toggle. The Editor publishes an
	 * *empty* list rather than null during normal operation, so GizmoPass early-outs
	 * on null and on empty alike.
	 */
	const GizmoDrawList* gizmoDraw = nullptr;

	// -----------------------------------------------------------------------
	// Renderer half — which frame. Stamped by DeferredRenderer::recordFrame().
	// -----------------------------------------------------------------------

	/// @brief Render area width in pixels (used for viewport, scissor, dispatch groups).
	uint32_t width = 0;

	/// @brief Render area height in pixels (used for viewport, scissor, dispatch groups).
	uint32_t height = 0;

	/// @brief Ring-buffer slot index for per-frame descriptor pools / UBOs.
	uint32_t frameIndex = 0;

	/// @brief Per-frame random 3D direction for shadow jitter (normalized unit-ball vector).
	/// Point lights use: pos_jittered = pos + light.radius * jitter
	/// Sun lights use: dir_jittered = normalize(lightDir + jitter * smallScale)
	glm::vec3 jitter{0.0f, 0.0f, 0.0f};

	/// @brief Per-frame iteration counter (incremented each frame, reset by Editor on scene changes).
	uint32_t iteration{0};

	// --- Null policy for the Editor half (see the file header) ---

	/** @brief True when a Scene was published; required before touching `scene`. */
	bool HasScene() const { return scene != nullptr; }

	/** @brief True when a viewing camera was published. */
	bool HasCamera() const { return camera != nullptr; }

	/**
	 * @brief This frame's render settings, or the built-in defaults.
	 *
	 * Never null, so a reader needs no branch and no fallback literal of its own.
	 * The fallback is core/RenderConfig.h's DefaultRenderConfig(), which *is*
	 * RenderConfig's member initializers rather than a second copy of them — the
	 * same answer UIContext::Settings() gives a UI reader, so the two contexts
	 * cannot drift apart on a default.
	 *
	 * Defined in RenderContext.cpp, since the complete type is needed there and
	 * this header deliberately only forward-declares RenderConfig.
	 */
	const RenderConfig& Settings() const;
};

} // namespace neurus
