/**
 * @file EditorViewport.h
 * @brief The editor's viewport: the camera it looks through, the world <-> screen
 *        conversions that camera defines, and the overlays drawn over it.
 *
 * Owns the viewport camera: pooled in the ResourceManager (so it is serialized
 * with the pool) but deliberately NOT scene content, so a scene with zero
 * cameras is a legal, fully renderable document. SyncCamera() chooses the
 * scene's *activated* camera when it names one and the viewport's own otherwise,
 * and GetCamera() is the single answer every consumer reads.
 *
 * Owns the overlay builders - debug flattening, gizmo state and gizmo geometry -
 * which compute against this camera and this extent; this class is their
 * composition root. The Scene only ever arrives as a per-frame parameter, never
 * owned, so nothing dangles across a scene swap.
 *
 * Everything here is in LOGICAL pixels: query results come out in the same unit
 * as the size handed in, and a world length maps to the same fraction of screen
 * height on either extent. ToRenderPixels() is the one deliberate exit to
 * physical texels, for ID-buffer readback.
 *
 * No Y flip lives here: the projection matrix alone carries the Vulkan NDC flip,
 * so adding one would silently drag a gizmo the wrong way while passing every
 * round-trip test.
 *
 * A null camera or a zero-sized viewport is normal during startup and teardown,
 * so every query returns a result carrying its own validity flag and nothing
 * ever throws or dereferences null.
 */

#pragma once

#include <memory>

#include <glm/glm.hpp>

#include "editor/viewport/DebugDrawBuilder.h"
#include "editor/viewport/GizmoDrawBuilder.h"
#include "editor/viewport/TransformGizmo.h"

namespace neurus
{

class Camera;
class ObjectID;
class ResourceManager;
class Scene;

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
	 * The clip-space w the perspective divide used: the quantity
	 * PixelsPerWorldUnit() needs, and free of any view-space handedness
	 * assumption.
	 */
	float viewDepth = 0.0f;

	/**
	 * @brief True when the point is in front of the near plane.
	 *
	 * It does NOT mean "inside the viewport" - an off-screen point in front of the
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

	/// @brief False when nothing projected - e.g. the whole box is behind the eye.
	bool valid = false;

	/**
	 * @brief True when a coordinate hit the guard band (see kGuardBandViewports).
	 *
	 * The rect is deliberately NOT clipped to the viewport - "this object extends
	 * off-screen to the left" is information callers need - but a box straddling
	 * the near plane can project arbitrarily far out, so coordinates are capped a
	 * few viewports away and this flag records that it happened.
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
	 * Falls straight out of unprojecting NDC z = 0, and every consumer treats the
	 * ray as a line, not as a pencil from a point.
	 */
	glm::vec3 origin{0.0f};

	/// @brief Unit direction pointing away from the camera.
	glm::vec3 direction{0.0f};

	/// @brief False when the viewport has no camera or no size.
	bool valid = false;
};

/**
 * @brief The editor's 3D viewport: its camera, its screen space, its overlays.
 *
 * The class implements the world <-> screen conversion itself; the mutators are
 * the Editor's alone, which is why controllers receive a `const EditorViewport*`.
 */
class EditorViewport
{
public:
	/// @brief Smallest clip-space w treated as being in front of the camera.
	static constexpr float kMinW = 1e-4f;

	/**
	 * @brief How many viewport widths/heights of slack the guard band allows.
	 *
	 * A projected coordinate is capped to the viewport plus four viewports of
	 * margin on every side: far enough out that no legitimate "just off-screen"
	 * answer is altered, close enough in to stay well inside float precision.
	 */
	static constexpr float kGuardBandViewports = 4.0f;

	/// @brief Where a freshly minted viewport camera stands, and what it looks at.
	static constexpr glm::vec3 kDefaultEye{0.0f, -5.0f, 2.0f};
	static constexpr glm::vec3 kDefaultTarget{0.0f, 0.0f, 0.0f};

	EditorViewport() = default;

	EditorViewport(const EditorViewport&) = delete;
	EditorViewport& operator=(const EditorViewport&) = delete;

	// -----------------------------------------------------------------------
	// The viewport camera - pooled, never scene content
	// -----------------------------------------------------------------------

	/**
	 * @brief Resolves, or creates, the viewport camera in the resource pool.
	 *
	 * Must run after every ResourceManager::Clear() - the pool owns the camera, so
	 * a clear drops it. A stored UID that resolves to a pooled Camera keeps the
	 * saved pose; otherwise a new pooled camera is created at the default views.
	 *
	 * The pool is a parameter rather than a stored reference, so nothing has to be
	 * wired up before anything could project a point.
	 */
	void EnsureCamera(ResourceManager& pool);

	/**
	 * @brief Drops the camera and its UID, because the pool that owned it was cleared.
	 *
	 * Keeping the stale UID instead would resolve, after a reload, to whatever
	 * object the pool later hands that id to.
	 */
	void ForgetCamera();

	/**
	 * @brief The viewport camera's pool UID (0 before the first scene exists).
	 *
	 * Registered in the project file so the viewport camera's identity survives
	 * save/load; the camera object itself lives in the serialized pool.
	 */
	int CameraID() const { return v_camUid; }

	/**
	 * @brief Restores the viewport camera's UID from a project file.
	 *
	 * Only records the id - the next EnsureCamera() resolves it against the pool
	 * and creates a fresh camera when the id names nothing.
	 */
	void RestoreCameraID(int uid) { v_camUid = uid; }

	/**
	 * @brief The viewport's own camera, or nullptr before EnsureCamera().
	 *
	 * Distinct from GetCamera(), which is whichever camera the current frame is
	 * rendered through - the two differ exactly while a scene camera is activated.
	 */
	Camera* ViewportCamera() { return v_camera.get(); }
	const Camera* ViewportCamera() const { return v_camera.get(); }

	/**
	 * @brief Chooses the camera this frame is rendered through, and frames it.
	 * @param scene Read for its *activated* camera only; never retained.
	 *
	 * The scene's activated camera wins, the viewport's own is the fallback;
	 * selection has nothing to do with it. Both candidates are brought up to the
	 * current extent, because activation can switch the view with no resize in
	 * between.
	 */
	void SyncCamera(Scene& scene);

	// -----------------------------------------------------------------------
	// Mutators - Editor only
	// -----------------------------------------------------------------------

	/**
	 * @brief Takes a viewport resize: the new extents, in both units.
	 * @param logical      Widget size in logical (Qt) pixels.
	 * @param renderExtent Swapchain size in physical pixels.
	 *
	 * Re-frames the viewport camera immediately, so a resize needs no frame to take
	 * effect, and dirties the gizmo guide, whose geometry is a pixel budget against
	 * the viewport height. A zero-sized extent is ignored rather than stored.
	 */
	void HandleResize(glm::uvec2 logical, glm::uvec2 renderExtent);

	/**
	 * @brief Sets both viewport extents.
	 * @param logical      Widget size in logical pixels; all query results use it.
	 * @param renderExtent Swapchain size in physical pixels; only the DPI
	 *                     conversion reads it.
	 *
	 * Both are taken together on purpose: they are two views of one resize, and
	 * accepting them separately is how they drift apart.
	 */
	void SetViewportSize(glm::uvec2 logical, glm::uvec2 renderExtent);

	/**
	 * @brief Publishes a camera to look through directly, bypassing SyncCamera().
	 * @param cam Borrowed, not owned; nullptr is legal and makes every query invalid.
	 *
	 * The primitive SyncCamera() is built on, and the way to point a viewport at a
	 * camera when there is no Scene to choose from. Borrowed rather than held: the
	 * pointed-at camera can be deleted between frames.
	 */
	void SetCamera(const Camera* cam);

	/**
	 * @brief Records the latest cursor position, in logical pixels.
	 *
	 * Dirties the gizmo guide while a gesture is live: its length and arc radius
	 * are fixed *pixel* budgets, so the world-space geometry changes with the
	 * cursor even when the state machine has not moved.
	 */
	void SetCursor(glm::vec2 logicalPixel);

	// -----------------------------------------------------------------------
	// Overlays - the debug flattener and the gizmo
	// -----------------------------------------------------------------------

	/**
	 * @brief Rebuilds both overlay payloads if they are dirty.
	 * @param scene Walked for debug objects and for the gizmo's anchor; never retained.
	 * @return True when at least one payload was actually rebuilt, i.e. this
	 *         frame's overlays differ from the ones the renderer last saw.
	 *
	 * The return value is the only question the Editor cannot answer itself: the
	 * dirty flags belong to the builders, and a renderer that has stopped after
	 * accumulating its samples has to be told to redraw the overlays. False is
	 * the common case and costs almost nothing.
	 */
	bool RebuildOverlays(const Scene& scene);

	/**
	 * @brief Marks both overlays stale: something visible changed.
	 *
	 * Marking is O(1); the rebuild happens once, in RebuildOverlays().
	 */
	void MarkOverlaysDirty();

	/**
	 * @brief Marks only the gizmo guide stale.
	 *
	 * Its geometry depends on more than scene content: a selection change moves the
	 * resting handle, and a camera or extent change rescales a pixel budget.
	 */
	void MarkGizmoDirty();

	/// @brief The flattened debug overlay payload.
	const DebugDrawList& DebugList() const { return v_debugDraw.List(); }

	/// @brief The gizmo guide geometry payload.
	const GizmoDrawList& GizmoList() const { return v_gizmoDraw.List(); }

	/// @brief The modal transform state machine.
	TransformGizmo& Gizmo() { return v_gizmo; }

	/**
	 * @brief True while a modal transform gesture is live.
	 *
	 * Read to gate the selection pick: a confirming click must not also re-select
	 * whatever is under the cursor.
	 */
	bool IsGizmoActive() const { return v_gizmo.IsActive(); }

	// -----------------------------------------------------------------------
	// Accessors
	// -----------------------------------------------------------------------

	/**
	 * @brief The camera this frame is rendered through, or nullptr.
	 *
	 * The viewport's own camera unless a scene camera is activated. Everything
	 * editor-side that needs a camera goes through here, which keeps every
	 * consumer ignorant of which of the two it has.
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
	 * Taken from the x axis; both axes share it in practice.
	 */
	float DeviceRatio() const;

	/// @brief True when a camera is set and the logical size is non-zero.
	bool IsValid() const;

	/**
	 * @brief The unit view direction, pointing from the eye into the scene.
	 * @return (0,0,0) when there is no camera or it looks at its own eye.
	 *
	 * The normal of every screen-parallel plane and the axis of every
	 * screen-parallel rotation. Lives here so the degenerate case is guarded once.
	 */
	glm::vec3 ViewAxis() const;

	// -----------------------------------------------------------------------
	// Queries
	// -----------------------------------------------------------------------

	/**
	 * @brief Converts logical pixels to physical render-target pixels.
	 *
	 * The one place a DPI factor belongs; every other query stays logical.
	 */
	glm::vec2 ToRenderPixels(glm::vec2 logical) const;

	/// @brief Projects a world point to a logical pixel. See ScreenPoint.
	ScreenPoint Project(const glm::vec3& world) const;

	/**
	 * @brief Builds the world-space ray through a logical pixel.
	 *
	 * The exact inverse of Project().
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
	 */
	ScreenRect ProjectBox(const glm::mat4& model,
	                      const glm::vec3& localMin,
	                      const glm::vec3& localMax) const;

private:
	/**
	 * @brief Fills @p out with the transform the gizmo handle should sit on.
	 * @return False when there is nothing to draw a handle for.
	 *
	 * Two sources, in order: the armed object (its transform keeps moving while
	 * the drag runs) and otherwise the active selection. The armed path cannot be
	 * replaced by the selection one, because a gesture survives a selection change.
	 */
	bool GizmoAnchor(const Scene& scene, TransformSnapshot& out) const;

	/// @brief Brings one camera up to the current logical extent, if it needs it.
	void FrameCamera(Camera* cam) const;

	/// @brief Borrowed camera this frame renders through. Never owned.
	const Camera* p_camera = nullptr;

	/**
	 * @brief The viewport's own camera, owned jointly with the ResourceManager pool.
	 *
	 * A shared_ptr because the pool hands out shared ownership; a Clear() there is
	 * why ForgetCamera()/EnsureCamera() exist.
	 */
	std::shared_ptr<Camera> v_camera;

	/// @brief The pool UID of v_camera; 0 when there is none. Survives save/load.
	int v_camUid = 0;

	/// @brief Logical (Qt) viewport size; the unit of every query result.
	glm::uvec2 v_logical{0u, 0u};

	/// @brief Physical render-target size; used only for the DPI conversion.
	glm::uvec2 v_render{0u, 0u};

	/// @brief Last cursor position in logical pixels.
	glm::vec2 v_cursor{0.0f};

	/// @brief Scene debug objects, flattened on a dirty flag into one payload.
	DebugDrawBuilder v_debugDraw;

	/// @brief The modal G/R/S state machine: what the gesture means.
	TransformGizmo v_gizmo;

	/// @brief The gizmo's picture, derived from v_gizmo and this viewport.
	GizmoDrawBuilder v_gizmoDraw;
};

// ---------------------------------------------------------------------------
// Free functions
//
// Kept out of the class so it stays ignorant of scene types. Declared here,
// defined in the .cpp.
// ---------------------------------------------------------------------------

/**
 * @brief Logical pixels a unit-length world segment spans at a given depth.
 * @param viewDepth ScreenPoint::viewDepth - the clip-space w, not a view-space Z.
 *
 * Closed form and aspect-independent by construction, which is what makes
 * constant-pixel gizmo geometry come out the same size on every window shape.
 * Returns 0 when the viewport is invalid.
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
 * Returns an invisible ScreenPoint for an object with no Transform3D rather than
 * guessing an origin, so callers cannot silently treat "has no position" as "is
 * at the world origin".
 */
ScreenPoint ScreenPosition(const EditorViewport& vp, const ObjectID& object);

} // namespace neurus
