/**
 * @file EditorContext.h
 * @brief Shared read-only snapshot of Editor-owned state (scene + config).
 *
 * EditorContext is the common editor state that both the Renderer and the UI
 * need each frame. It is produced once by Editor::GetContext() and embedded by
 * value into both RenderContext (render layer) and UIContext (UI layer), so the
 * Editor exposes a single source of truth instead of building each context
 * separately.
 *
 * Architecture:
 * - Lives in the Vulkan-free scene layer so both render/ and ui/ may include it.
 * - `scene` is the Editor-owned Scene upcast to its UID base; consumers cast it
 *   back to `const Scene*`.
 * - `config` stays an opaque `const void*` (a RenderConfig*) so this header does
 *   not pull the renderer's RenderConfig into the UI layer.
 * - `camera` is the one Editor-side answer to "which camera are we looking
 *   through". It comes from EditorViewport, not from Scene::GetActiveCamera(),
 *   which is what lets a future viewport-owned free camera change one function
 *   body instead of every consumer.
 * - Every pointer here is borrowed and frame-scoped: valid only for the frame
 *   Editor::GetContext() produced it in, never stored across frames.
 */

#pragma once

#include "core/UID.h"

namespace neurus
{

class Camera;
struct DebugDrawList;
struct GizmoDrawList;

/**
 * @brief Editor-owned scene + render config, shared by RenderContext/UIContext.
 */
struct EditorContext
{
	/// @brief Editor-owned Scene (upcast to UID). Cast to const Scene* to use.
	const UID* scene = nullptr;

	/// @brief Opaque RenderConfig*. Cast to const RenderConfig* to read flags.
	const void* config = nullptr;

	/**
	 * @brief The camera this frame is being viewed through, or nullptr.
	 *
	 * Published from EditorViewport::GetCamera(), which is deliberately the only
	 * Editor-side definition of the viewing camera: the gizmo's projection math
	 * and the renderer's CameraGPU read the same pointer, so they cannot disagree.
	 * Today it is the Scene's active camera; when the viewport owns a free camera
	 * it will not be, and nothing downstream needs to change.
	 */
	const Camera* camera = nullptr;

	/**
	 * @brief This frame's debug geometry, or nullptr when debug draw is off.
	 *
	 * Owned by the Editor and rebuilt every Edit(); valid only for the frame it
	 * was published in. Forward-declared rather than included so this header
	 * stays free of glm/<vector> for the UI layer — include
	 * scene/DebugDrawList.h where the contents are actually read.
	 */
	const DebugDrawList* debugDraw = nullptr;

	/**
	 * @brief This frame's transform-gizmo guides; empty while no gesture is active.
	 *
	 * Separate from `debugDraw` because the two have opposite lifetimes and
	 * opposite visibility rules — see scene/GizmoDrawList.h. There is deliberately
	 * no RenderConfig flag gating it: interaction feedback must not be hideable by a
	 * debug-visualization toggle.
	 *
	 * The Editor publishes &GizmoDrawBuilder::List() unconditionally, so during
	 * normal operation this is non-null and *empty* rather than null when no gesture
	 * is in progress — the builder's "empty, never null" contract. It stays a
	 * pointer because a default-constructed EditorContext (a test driving a pass
	 * directly, a frame recorded before the Editor has published anything) has no
	 * list at all, and GizmoPass early-outs on null and on empty alike.
	 */
	const GizmoDrawList* gizmoDraw = nullptr;
};

} // namespace neurus
