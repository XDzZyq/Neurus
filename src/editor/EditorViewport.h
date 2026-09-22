/**
 * @file EditorViewport.h
 * @brief Editor-layer world <-> screen space service for the 3D viewport.
 *
 * One object answers every "where is this on screen" and "what is under the
 * cursor" question in the editor. Before it existed, that knowledge was spread
 * across the Editor's viewport width/height members, an open-coded device-ratio
 * multiply in the Application's pick path, and whatever each caller derived from
 * Scene::GetActiveCamera() — three places that could disagree.
 *
 * Two future features shaped this API more than the transform gizmo did:
 *
 *   1. The viewport will own a free camera instead of borrowing the Scene's
 *      active one. That is why the camera is *pushed in* per frame rather than
 *      fetched from a Scene, and why GetCamera() exists: it is the single
 *      Editor-side definition of "the camera we are looking through", consumed by
 *      the gizmo and republished to the renderer through EditorContext::camera.
 *      Swapping in a viewport-owned camera changes one function body.
 *   2. The viewport will answer object queries (screen position, screen bounding
 *      box). Those are free functions below rather than members, so the class
 *      stays ignorant of scene types and testable against a bare Camera.
 *
 * Layer placement: editor, Vulkan-free and Qt-free. It reads Camera (scene layer)
 * and nothing else.
 *
 * ## Everything here is in LOGICAL pixels
 *
 * Camera::cam_w/cam_h are logical Qt pixels; the renderer's swapchain extent is
 * physical. This is not a discrepancy that needs a DPI factor sprinkled through
 * the math: Camera::RecomputeMatrices() puts aspect in proj[0][0] and the FOV term
 * in proj[1][1], so |proj[1][1]| is *identical* whichever extent you build the
 * camera from, and a world length therefore maps to the same fraction of screen
 * height either way. Pixel results simply come out in the same unit as the size
 * you were given. ToRenderPixels() is the one deliberate exit from that world, for
 * the ID-buffer readback that must address physical texels.
 *
 * ## No Y flip lives here
 *
 * Camera.cpp applies proj[1][1] *= -1 for Vulkan NDC (y = -1 at the top). Mapping
 * NDC to a top-left-origin pixel with (ndc * 0.5 + 0.5) * size therefore needs no
 * flip of its own, and adding one would silently drag a gizmo the wrong way while
 * passing every round-trip test. There is exactly one flip in the whole chain and
 * it is in the projection matrix.
 *
 * ## Validity is data, not an exception
 *
 * A null camera or a zero-sized viewport is normal during startup and teardown, so
 * every query returns a result carrying its own validity flag and nothing ever
 * throws or dereferences null. Callers check the flag.
 */

#pragma once

#include <glm/glm.hpp>

namespace neurus
{

class Camera;
class ObjectID;

/**
 * @brief A world point projected to the screen.
 */
struct ScreenPoint
{
	/// @brief Logical-pixel position, top-left origin. Meaningless when !visible.
	glm::vec2 pixel{0.0f};

	/**
	 * @brief Distance from the eye along the view axis, in world units.
	 *
	 * This is the clip-space w the perspective divide used, which is the quantity
	 * PixelsPerWorldUnit() needs and the same one overlay_point.vert divides by.
	 * Taking w rather than a view-space Z keeps the whole class free of any
	 * assumption about view-space handedness.
	 */
	float viewDepth = 0.0f;

	/**
	 * @brief True when the point is in front of the near plane.
	 *
	 * It does NOT mean "inside the viewport" — an off-screen point in front of the
	 * camera is visible with pixel coordinates outside [0, size].
	 */
	bool visible = false;
};

/**
 * @brief An axis-aligned screen-space rectangle, in logical pixels.
 */
struct ScreenRect
{
	glm::vec2 min{0.0f};  ///< Top-left corner (smaller x, smaller y).
	glm::vec2 max{0.0f};  ///< Bottom-right corner.

	/// @brief False when nothing projected — e.g. the whole box is behind the eye.
	bool valid = false;

	/**
	 * @brief True when a coordinate hit the guard band (see kGuardBandViewports).
	 *
	 * The rect is deliberately NOT clipped to the viewport — "this object extends
	 * off-screen to the left" is information callers need. But a box straddling the
	 * near plane can project arbitrarily far out, so coordinates are capped a few
	 * viewports away to keep them finite and this flag records that it happened.
	 */
	bool clamped = false;
};

/**
 * @brief A world-space ray cast through a screen pixel.
 */
struct ScreenRay
{
	/**
	 * @brief A point on the ray, on the near plane rather than at the eye.
	 *
	 * Near-plane rather than eye position because it falls straight out of
	 * unprojecting NDC z = 0 and needs no camera-position accessor, and because
	 * every consumer treats the ray as a line, not as a pencil from a point.
	 */
	glm::vec3 origin{0.0f};

	/// @brief Unit direction pointing away from the camera.
	glm::vec3 direction{0.0f};

	/// @brief False when the viewport has no camera or no size.
	bool valid = false;
};

/**
 * @brief World <-> screen projection service for the editor's 3D viewport.
 *
 * The public surface is wide (three setters, five trivial accessors, four
 * queries) but the responsibility is single: convert between world space and this
 * viewport's screen space. The setters are the Editor's alone — the same contract
 * the mutable Scene and RenderConfig providers in ControllerContext already carry
 * — which is why controllers receive a `const EditorViewport*`.
 */
class EditorViewport
{
public:
	/// @brief Smallest clip-space w treated as being in front of the camera.
	static constexpr float kMinW = 1e-4f;

	/**
	 * @brief How many viewport widths/heights of slack the guard band allows.
	 *
	 * A projected coordinate is capped to [-4*size, +5*size] — the viewport itself
	 * plus four viewports of margin on every side. Far enough out that no
	 * legitimate "just off-screen" answer is altered, close enough in that the
	 * numbers stay well inside float precision for whatever the caller multiplies
	 * them by.
	 */
	static constexpr float kGuardBandViewports = 4.0f;

	// -----------------------------------------------------------------------
	// Mutators — Editor only
	// -----------------------------------------------------------------------

	/**
	 * @brief Sets both viewport extents.
	 * @param logical      Widget size in logical pixels; all query results use it.
	 * @param renderExtent Swapchain size in physical pixels; only ToRenderPixels
	 *                     and DeviceRatio read it.
	 *
	 * Both are taken together on purpose: they are two views of one resize, and
	 * accepting them separately is how they drift apart.
	 */
	void SetViewportSize(glm::uvec2 logical, glm::uvec2 renderExtent);

	/**
	 * @brief Publishes the camera to look through for the coming frame.
	 * @param cam Borrowed, not owned; nullptr is legal and makes every query invalid.
	 *
	 * Pushed every frame rather than held long-term, because the pointed-at camera
	 * can be deleted between frames. Nothing here outlives one frame's use.
	 */
	void SetCamera(const Camera* cam);

	/// @brief Records the latest cursor position, in logical pixels.
	void SetCursor(glm::vec2 logicalPixel);

	// -----------------------------------------------------------------------
	// Accessors
	// -----------------------------------------------------------------------

	/**
	 * @brief The camera this viewport looks through, or nullptr.
	 *
	 * The seam the planned free camera replaces. Everything editor-side that needs
	 * a camera goes through here instead of Scene::GetActiveCamera().
	 */
	const Camera* GetCamera() const { return p_camera; }

	/// @brief Viewport size in logical pixels.
	glm::uvec2 Size() const { return v_logical; }

	/// @brief Render target size in physical pixels.
	glm::uvec2 RenderExtent() const { return v_render; }

	/// @brief Last known cursor position, in logical pixels.
	glm::vec2 Cursor() const { return v_cursor; }

	/**
	 * @brief Physical pixels per logical pixel (1.0 on a non-HiDPI display).
	 *
	 * Taken from the x axis; both axes share it in practice. ToRenderPixels scales
	 * per axis anyway, so it stays exact even if they ever diverge.
	 */
	float DeviceRatio() const;

	/// @brief True when a camera is set and the logical size is non-zero.
	bool IsValid() const;

	// -----------------------------------------------------------------------
	// Queries
	// -----------------------------------------------------------------------

	/**
	 * @brief Converts logical pixels to physical render-target pixels.
	 *
	 * The one place a DPI factor belongs: the ID-buffer readback samples a
	 * physical texel. Every other query stays logical.
	 */
	glm::vec2 ToRenderPixels(glm::vec2 logical) const;

	/// @brief Projects a world point to a logical pixel. See ScreenPoint.
	ScreenPoint Project(const glm::vec3& world) const;

	/**
	 * @brief Builds the world-space ray through a logical pixel.
	 *
	 * The exact inverse of Project(): unprojecting a pixel Project() produced gives
	 * a ray passing through the original world point.
	 */
	ScreenRay RayThrough(glm::vec2 logicalPixel) const;

	/**
	 * @brief Screen-space bounds of a transformed local box, near-plane correct.
	 * @param model    Object-to-world matrix; the 8 local corners go through
	 *                 viewProj * model, which is tighter than re-fitting a world
	 *                 AABB first for a rotated box.
	 * @param localMin Box minimum in the model's own space.
	 * @param localMax Box maximum in the model's own space.
	 *
	 * Exact rather than conservative: the projection of a convex box is the convex
	 * hull of its projected vertices, so the extremes are attained at vertices (or,
	 * for a box straddling the eye, where its edges cross the near plane).
	 *
	 * The box is taken explicitly instead of read off a mesh so this is fully
	 * testable before any AABB exists in the asset layer.
	 */
	ScreenRect ProjectBox(const glm::mat4& model,
	                      const glm::vec3& localMin,
	                      const glm::vec3& localMax) const;

private:
	/// @brief Borrowed camera, re-pushed every frame. Never owned.
	const Camera* p_camera = nullptr;

	/// @brief Logical (Qt) viewport size; the unit of every query result.
	glm::uvec2 v_logical{0u, 0u};

	/// @brief Physical render-target size; used only for the DPI conversion.
	glm::uvec2 v_render{0u, 0u};

	/// @brief Last cursor position in logical pixels.
	glm::vec2 v_cursor{0.0f};
};

// ---------------------------------------------------------------------------
// Free functions
//
// Kept out of the class so EditorViewport needs no knowledge of scene types and
// so an object-level query cannot be called against a stand-in that fakes the
// projection. They are declared here and defined in the .cpp.
// ---------------------------------------------------------------------------

/**
 * @brief Logical pixels a unit-length world segment spans at a given depth.
 * @param viewDepth ScreenPoint::viewDepth — the clip-space w, not a view-space Z.
 *
 * Closed form: |proj[1][1]| * height * 0.5 / viewDepth. Aspect-independent by
 * construction, which is what makes constant-pixel gizmo geometry come out the
 * same size on every window shape. Returns 0 when the viewport is invalid.
 */
float PixelsPerWorldUnit(const EditorViewport& vp, float viewDepth);

/**
 * @brief Screen bounds of a world-space AABB. Thin wrapper over ProjectBox.
 */
ScreenRect ProjectBounds(const EditorViewport& vp,
                         const glm::vec3& worldMin,
                         const glm::vec3& worldMax);

/**
 * @brief Screen position of any scene object that carries a transform.
 *
 * Returns an invisible ScreenPoint for an object with no Transform3D (an
 * Environment, say) rather than guessing an origin, so callers cannot silently
 * treat "has no position" as "is at the world origin".
 *
 * Deliberately absent: a ScreenBounds() taking an ObjectID. Nothing in the asset
 * layer produces a local AABB yet, and declaring the query before a real bounds
 * source exists would invite callers to test it against a fabricated box.
 */
ScreenPoint ScreenPosition(const EditorViewport& vp, const ObjectID& object);

} // namespace neurus
