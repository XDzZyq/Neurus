/**
 * @file SceneContext.h
 * @brief Shared read-only snapshot of what is being looked at this frame.
 *
 * The per-frame state that both the Renderer and the UI need, produced once by
 * Editor::GetContext() and embedded by value in both RenderContext and
 * UIContext, so the Editor is the single source of truth for the frame's view.
 *
 * Every member is a borrowed, frame-scoped pointer, named by forward
 * declaration: valid only for the frame it was produced in, never stored across
 * frames. Every consumer must check.
 *
 * Null policy:
 * - `config` is always safe through Settings(), which falls back to
 *   RenderConfig's own in-class defaults. Read it that way and never branch.
 * - `scene`, `camera`, `debugDraw` and `gizmoDraw` must be checked, via
 *   HasScene() / HasCamera() or a plain null test.
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
	/// @brief The Scene being rendered, or nullptr. Check HasScene().
	const Scene* scene = nullptr;

	/**
	 * @brief The render settings, or nullptr.
	 *
	 * Read it through Settings(), never directly: that keeps RenderConfig's
	 * in-class member initializers the only place a default is written.
	 */
	const RenderConfig* config = nullptr;

	/**
	 * @brief The camera this frame is being viewed through, or nullptr.
	 *
	 * The single definition of the viewing camera, so every consumer that
	 * builds a view-projection or centres a shadow frustum reads the same
	 * pointer and they cannot disagree. When no scene camera is active the
	 * viewport falls back to its own camera.
	 */
	const Camera* camera = nullptr;

	/**
	 * @brief This frame's debug geometry, or nullptr when debug draw is off.
	 *
	 * Rebuilt every Edit(); forward-declared rather than included so this
	 * header stays free of glm/<vector> for the UI layer - include
	 * scene/DebugDrawList.h where the contents are actually read.
	 */
	const DebugDrawList* debugDraw = nullptr;

	/**
	 * @brief This frame's transform-gizmo geometry; empty when nothing is selected.
	 *
	 * Carries the resting translate handle on the selection's active object, or
	 * the armed gesture's guide while one is live.
	 *
	 * There is deliberately no config flag gating it: interaction feedback must
	 * not be hideable by a debug-visualization toggle. Published
	 * unconditionally, so during normal operation this is non-null and empty
	 * rather than null when there is nothing to draw.
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
	 * Never null, so a reader needs no branch and no fallback literal of its
	 * own. Defined in SceneContext.cpp, which needs the complete RenderConfig
	 * type that this header only forward-declares.
	 */
	const RenderConfig& Settings() const;
};

} // namespace neurus
