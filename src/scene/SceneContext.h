/**
 * @file SceneContext.h
 * @brief Shared read-only snapshot of what is being looked at this frame.
 *
 * SceneContext is the per-frame state that both the Renderer and the UI need.
 * It is produced once by Editor::GetContext() and embedded by value into both
 * RenderContext (render layer) and UIContext (UI layer), so the Editor exposes
 * a single source of truth instead of building each context separately.
 *
 * Architecture:
 * - Lives in the Vulkan-free scene layer so both render/ and ui/ may include it.
 *   The name follows the directory: neither consumer should have to say "Editor"
 *   to ask what the frame is looking at.
 * - **Every member is a typed pointer named by forward declaration.** The header
 *   therefore pulls in nothing, yet the struct *is* the manifest of what a frame
 *   carries, and no consumer casts. Two members used to be type-erased
 *   (`const UID* scene`, `const void* config`); that cost 18 `static_cast` sites,
 *   a null policy re-decided at each one — two of them wrong — and five
 *   restatements of a RenderConfig default, one of which had silently drifted 40x.
 *   The erasure protected nothing: every scene/ and core/ header is Vulkan-free
 *   and Qt-free, and the UI layer already includes scene/Scene.h directly.
 * - **Growth is composition, not erasure.** Adding something to the frame's scene
 *   view is one forward declaration plus one field. A component that is *not* part
 *   of that view belongs in a sibling struct embedded next to this one, so neither
 *   struct becomes the god struct, and never in a `void*` slot. (Blender's
 *   bContext carries ~40 members this way, forward-declared in one header; where
 *   it does erase, it pays for a runtime type check against RNA. We have no RNA,
 *   so we stay in the typed tier.)
 * - Every pointer here is borrowed and frame-scoped: valid only for the frame
 *   Editor::GetContext() produced it in, never stored across frames.
 *
 * Null policy — what a consumer may assume, and what it must check:
 * - `config` is **always safe** when reached through Settings(), which falls back
 *   to RenderConfig's own in-class defaults. Read it that way and never branch.
 * - `scene`, `camera`, `debugDraw` and `gizmoDraw` **must be checked**, via
 *   HasScene() / HasCamera() or a plain null test. A frame recorded before a
 *   scene exists is legal — that is exactly what a test driving a pass with a
 *   default-constructed context reproduces — so a pass that dereferences without
 *   checking is a crash waiting for that frame.
 */

#pragma once

namespace neurus
{

class Camera;
class RenderConfig;
class Scene;
struct DebugDrawList;
struct GizmoDrawList;

/**
 * @brief The frame's scene view, shared by RenderContext and UIContext.
 */
struct SceneContext
{
	/// @brief The Editor-owned Scene being rendered, or nullptr. Check HasScene().
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
	 * VulkanTestShared::PublishSceneCamera(cache, ctx.snapshot) fills this and the
	 * CameraGPU together.
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
	 * @brief This frame's transform-gizmo geometry; empty when nothing is selected.
	 *
	 * Carries the resting translate handle on the selection's active object, or the
	 * armed gesture's guide while one is live.
	 *
	 * Separate from `debugDraw` because the two have opposite lifetimes and
	 * opposite visibility rules — see scene/GizmoDrawList.h. There is deliberately
	 * no RenderConfig flag gating it: interaction feedback must not be hideable by a
	 * debug-visualization toggle.
	 *
	 * The Editor publishes &GizmoDrawBuilder::List() unconditionally, so during
	 * normal operation this is non-null and *empty* rather than null when there is
	 * nothing to draw — the builder's "empty, never null" contract. It stays a
	 * pointer because a default-constructed SceneContext (a test driving a pass
	 * directly, a frame recorded before the Editor has published anything) has no
	 * list at all, and GizmoPass early-outs on null and on empty alike.
	 */
	const GizmoDrawList* gizmoDraw = nullptr;

	// --- Null policy (see the file header) ---

	/** @brief True when a Scene was published; required before touching `scene`. */
	bool HasScene() const { return scene != nullptr; }

	/** @brief True when a viewing camera was published. */
	bool HasCamera() const { return camera != nullptr; }

	/**
	 * @brief This frame's render settings, or the built-in defaults.
	 *
	 * Never null, so a reader needs no branch and no fallback literal of its own.
	 * The fallback is a default-constructed RenderConfig, which means it *is*
	 * RenderConfig's member initializers rather than a second copy of them.
	 * Defined in SceneContext.cpp, since the complete type is needed there and
	 * this header deliberately only forward-declares it.
	 */
	const RenderConfig& Settings() const;
};

} // namespace neurus
