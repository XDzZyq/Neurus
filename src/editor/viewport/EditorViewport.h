/**
 * @file EditorViewport.h
 * @brief The editor's viewport: the camera it looks through, the world <-> screen
 *        conversions that camera defines, and the overlays drawn over it.
 *
 * One object answers every "where is this on screen" and "what is under the
 * cursor" question in the editor. Before it existed, that knowledge was spread
 * across the Editor's viewport width/height members, an open-coded device-ratio
 * multiply in the Application's pick path, and whatever each caller derived from
 * Scene::GetActiveCamera() — three places that could disagree.
 *
 * ## It owns the viewport camera, so the Editor knows nothing about one
 *
 * The viewport camera is created and kept here: pooled in the ResourceManager (so
 * it is serialized with the pool, and CameraController resolves it by UID like any
 * other camera) but deliberately NOT scene content — it never enters
 * Scene::cam_list, so it cannot be selected, deleted or activated. That is what
 * makes a scene with zero cameras a legal, fully renderable document.
 *
 * Which camera a frame is rendered through is a viewport question, not an Editor
 * one: SyncCamera() takes the scene's *activated* camera when it names one and the
 * viewport's own otherwise, and GetCamera() is the single answer everything else
 * reads — the gizmo's projection math, every pick, and EditorContext::camera.
 *
 * ## It owns the overlays, for the same reason
 *
 * DebugDrawBuilder, TransformGizmo and GizmoDrawBuilder all compute against this
 * camera and this extent — a fixed *pixel* budget means nothing without them. They
 * are members here and keep their own single responsibilities: this class is their
 * composition root, not their implementation, which is why the surface it adds for
 * them is four forwarding calls and two list accessors.
 *
 * The one thing it does NOT own is the Scene. That arrives as a parameter to the
 * two per-frame calls that need it, so nothing here dangles across File > New or
 * File > Open and the class stays ignorant of what a scene contains.
 *
 * The second feature that shaped this API: object queries (screen position, screen
 * bounding box) are free functions below rather than members, so the class stays
 * ignorant of scene types and is testable against a bare Camera.
 *
 * Layer placement: editor, Vulkan-free and Qt-free.
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
 * @brief The editor's 3D viewport: its camera, its screen space, its overlays.
 *
 * The surface is wide because the viewport is where several thin things meet — a
 * camera lifecycle, an extent, a cursor, four projection queries and three overlay
 * collaborators — but each of them is a one-liner over a member that keeps its own
 * single responsibility. What the class itself implements is only the world <->
 * screen conversion.
 *
 * The mutators are the Editor's alone — the same contract the mutable Scene and
 * RenderConfig providers in ControllerContext already carry — which is why
 * controllers receive a `const EditorViewport*`.
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

	/// @brief Where a freshly minted viewport camera stands, and what it looks at.
	static constexpr glm::vec3 kDefaultEye{0.0f, -5.0f, 2.0f};
	static constexpr glm::vec3 kDefaultTarget{0.0f, 0.0f, 0.0f};

	EditorViewport() = default;

	EditorViewport(const EditorViewport&) = delete;
	EditorViewport& operator=(const EditorViewport&) = delete;

	// -----------------------------------------------------------------------
	// The viewport camera — pooled, never scene content
	// -----------------------------------------------------------------------

	/**
	 * @brief Resolves, or creates, the viewport camera in the resource pool.
	 *
	 * Must run after every ResourceManager::Clear() — the pool owns the camera, so
	 * a clear drops it. When the stored UID resolves to a pooled Camera (a loaded
	 * project) the saved pose is kept; otherwise a new pooled camera is created at
	 * kDefaultEye/kDefaultTarget, already framed to the current extent.
	 *
	 * The pool is a parameter rather than a stored reference: these two calls are
	 * the only place the viewport needs one, and a member would have to be wired up
	 * before anything could project a point.
	 */
	void EnsureCamera(ResourceManager& pool);

	/**
	 * @brief Drops the camera and its UID, because the pool that owned it was cleared.
	 *
	 * The scene-swap paths call this before ResourceManager::Clear() mints anything
	 * new. Keeping the stale UID instead would resolve, after a reload, to whatever
	 * object the pool later hands that id to.
	 */
	void ForgetCamera();

	/**
	 * @brief The viewport camera's pool UID (0 before the first scene exists).
	 *
	 * The Application registers a project::EditorComponent against this pair so the
	 * viewport camera's identity survives save/load. The camera *object* needs no
	 * help: it lives in the ResourceManager pool, which is serialized whole.
	 */
	int CameraID() const { return v_camUid; }

	/**
	 * @brief Restores the viewport camera's UID from a project file.
	 *
	 * Only records the id — the next EnsureCamera() resolves it against the pool
	 * (and creates a fresh camera when the id names nothing, e.g. a project written
	 * before this field existed).
	 */
	void RestoreCameraID(int uid) { v_camUid = uid; }

	/**
	 * @brief The viewport's own camera, or nullptr before EnsureCamera().
	 *
	 * Distinct from GetCamera(), which is whichever camera the current frame is
	 * rendered through — the two differ exactly while a scene camera is activated.
	 */
	Camera* ViewportCamera() { return v_camera.get(); }
	const Camera* ViewportCamera() const { return v_camera.get(); }

	/**
	 * @brief Chooses the camera this frame is rendered through, and frames it.
	 * @param scene Read for its *activated* camera only; never retained.
	 *
	 * The scene's activated camera wins, the viewport's own is the fallback — which
	 * is the whole of "an activated scene camera takes the view". Selection has
	 * nothing to do with it: selecting a scene camera does not change the view.
	 *
	 * Both candidates are brought up to the current extent, not just the one chosen:
	 * activation can switch the view with no resize in between, and a camera that
	 * missed one would present a stretched image the moment it took over. A
	 * *deactivated* scene camera is deliberately left alone — it adopts the extent
	 * when it becomes the view camera, not before.
	 *
	 * Called at the top of Editor::Edit() and again after the event queue drains,
	 * because activating, deactivating or deleting a camera all land inside that
	 * drain.
	 */
	void SyncCamera(Scene& scene);

	// -----------------------------------------------------------------------
	// Mutators — Editor only
	// -----------------------------------------------------------------------

	/**
	 * @brief Takes a viewport resize: the new extents, in both units.
	 * @param logical      Widget size in logical (Qt) pixels.
	 * @param renderExtent Swapchain size in physical pixels.
	 *
	 * The entry point for the resize event, as opposed to SetViewportSize() which is
	 * the bare setter underneath it: this also re-frames the viewport camera
	 * immediately (so a resize needs no frame to take effect) and dirties the gizmo
	 * guide, whose geometry is a pixel budget against the viewport height. A
	 * zero-sized extent is ignored rather than stored — it would divide by zero in
	 * the projection, and it is what the window reports before it is first shown.
	 *
	 * The activated scene camera, if any, is re-framed by the next SyncCamera():
	 * that one needs a Scene and runs before any draw.
	 */
	void HandleResize(glm::uvec2 logical, glm::uvec2 renderExtent);

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
	 * @brief Publishes a camera to look through directly, bypassing SyncCamera().
	 * @param cam Borrowed, not owned; nullptr is legal and makes every query invalid.
	 *
	 * The primitive SyncCamera() is built on, and the way to point a viewport at a
	 * camera when there is no Scene to choose from — a projection query needs
	 * nothing else. Borrowed rather than held: the pointed-at camera can be deleted
	 * between frames, so nothing here outlives one frame's use.
	 */
	void SetCamera(const Camera* cam);

	/**
	 * @brief Records the latest cursor position, in logical pixels.
	 *
	 * Dirties the gizmo guide while a gesture is live: the guide's length and arc
	 * radius are fixed *pixel* budgets converted through PixelsPerWorldUnit(), so
	 * the world-space geometry changes with the cursor even when the state machine
	 * has not moved. A resting handle is anchored on the object and sized from the
	 * camera, so an idle mouse sweep cannot change it.
	 */
	void SetCursor(glm::vec2 logicalPixel);

	// -----------------------------------------------------------------------
	// Overlays — the debug flattener and the gizmo
	// -----------------------------------------------------------------------

	/**
	 * @brief Rebuilds both overlay payloads if they are dirty.
	 * @param scene Walked for debug objects and for the gizmo's anchor; never retained.
	 *
	 * Runs at the end of Editor::Edit(), after the event queue drains, so a scene
	 * change and its overlay consequences land in the same frame. Returns almost
	 * immediately when clean, which is the common case.
	 */
	void RebuildOverlays(const Scene& scene);

	/**
	 * @brief Marks both overlays stale: something visible changed.
	 *
	 * Driven by RenderResetEvent — the codebase's existing "something visible
	 * changed" broadcast — and by every scene swap. Marking is O(1); the rebuild
	 * happens once, in RebuildOverlays().
	 */
	void MarkOverlaysDirty();

	/**
	 * @brief Marks only the gizmo guide stale.
	 *
	 * Its geometry depends on more than scene content: a selection change moves the
	 * resting handle, and a camera or extent change rescales a pixel budget.
	 */
	void MarkGizmoDirty();

	/// @brief The flattened debug overlay, published through EditorContext::debugDraw.
	const DebugDrawList& DebugList() const { return v_debugDraw.List(); }

	/// @brief The gizmo guide geometry, published through EditorContext::gizmoDraw.
	const GizmoDrawList& GizmoList() const { return v_gizmoDraw.List(); }

	/// @brief The modal transform state machine, for TransformGizmoController to drive.
	TransformGizmo& Gizmo() { return v_gizmo; }

	/**
	 * @brief True while a modal transform gesture is live.
	 *
	 * Read by the Application to gate the viewport's selection pick: a confirming
	 * LMB click must not also re-select whatever is under the cursor, and must not
	 * pay that path's queue.waitIdle ID-buffer readback. Frame-granular and
	 * therefore safe, since gizmo state only ever changes inside Editor::Edit().
	 */
	bool IsGizmoActive() const { return v_gizmo.IsActive(); }

	// -----------------------------------------------------------------------
	// Accessors
	// -----------------------------------------------------------------------

	/**
	 * @brief The camera this frame is rendered through, or nullptr.
	 *
	 * The viewport's own camera unless a scene camera is activated (SyncCamera()).
	 * Everything editor-side that needs a camera goes through here instead of
	 * Scene::GetActiveCamera(), which is what keeps every consumer — the gizmo, the
	 * picks, every render pass — ignorant of which of the two it has.
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

	/**
	 * @brief The unit view direction, pointing from the eye into the scene.
	 * @return (0,0,0) when there is no camera or it looks at its own eye.
	 *
	 * normalize(cam_tar - eye), which is the normal of every screen-parallel plane
	 * and the axis of every screen-parallel rotation. Lives here rather than in each
	 * caller so the degenerate case is guarded once: callers test the length instead
	 * of re-deriving the camera terms and re-choosing an epsilon.
	 */
	glm::vec3 ViewAxis() const;

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
	/**
	 * @brief Fills @p out with the transform the gizmo handle should sit on.
	 * @return False when there is nothing to draw a handle for.
	 *
	 * Two sources, in order: the armed object (the gesture owns it by UID, and its
	 * transform keeps moving while the drag runs) and otherwise the active
	 * selection. The UID path cannot be replaced by the selection one — a gesture
	 * survives a selection change, and reading the selection mid-drag would move
	 * the handle to a different object.
	 */
	bool GizmoAnchor(const Scene& scene, TransformSnapshot& out) const;

	/// @brief Brings one camera up to the current logical extent, if it needs it.
	void FrameCamera(Camera* cam) const;

	/// @brief Borrowed camera this frame renders through. Never owned.
	const Camera* p_camera = nullptr;

	/**
	 * @brief The viewport's own camera, owned jointly with the ResourceManager pool.
	 *
	 * A shared_ptr because the pool hands out shared ownership; the pool is what
	 * serializes it, and a Clear() there is why ForgetCamera()/EnsureCamera() exist.
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
