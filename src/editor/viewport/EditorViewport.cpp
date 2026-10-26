/**
 * @file EditorViewport.cpp
 * @brief Implementation of the editor's viewport: camera, screen space, overlays.
 *
 * The only subtle part is ProjectBox's near-plane handling, which is easy to get
 * wrong silently - see ClipBoxEdges below.
 */

#include "editor/viewport/EditorViewport.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/Log.h"
#include "core/ResourceManager.h"
#include "scene/Camera.h"
#include "scene/ObjectID.h"
#include "scene/Scene.h"
#include "scene/Transform.h"

namespace neurus
{

namespace
{

/// @brief Below this, cam_tar - eye has no meaningful direction to normalize.
constexpr float kMinViewAxisLength = 1e-3f;

/**
 * @brief The 8 corners of a local AABB, indexed so bit 0 = x, bit 1 = y, bit 2 = z.
 *
 * The bit encoding makes the edge list trivial: two corners share an edge exactly
 * when their indices differ in one bit.
 */
std::array<glm::vec3, 8> BoxCorners(const glm::vec3& lo, const glm::vec3& hi)
{
	std::array<glm::vec3, 8> c{};
	for (int i = 0; i < 8; ++i)
	{
		c[i] = glm::vec3((i & 1) ? hi.x : lo.x,
		                 (i & 2) ? hi.y : lo.y,
		                 (i & 4) ? hi.z : lo.z);
	}
	return c;
}

/// @brief The 12 box edges as index pairs differing in exactly one corner bit.
constexpr int kBoxEdges[12][2] = {
	{0, 1}, {2, 3}, {4, 5}, {6, 7},  // along x
	{0, 2}, {1, 3}, {4, 6}, {5, 7},  // along y
	{0, 4}, {1, 5}, {2, 6}, {3, 7},  // along z
};

/// @brief Clip position -> logical pixel, top-left origin. Assumes clip.w > 0.
glm::vec2 PixelOf(const glm::vec4& clip, const glm::vec2& size)
{
	const glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
	return (ndc * 0.5f + 0.5f) * size;
}

} // namespace

// ---------------------------------------------------------------------------
// The viewport camera
// ---------------------------------------------------------------------------

void EditorViewport::EnsureCamera(ResourceManager& pool)
{
	if (v_camUid != 0)
	{
		v_camera = pool.Get<Camera>(v_camUid);
		if (v_camera)
		{
			// A loaded project: the pose is the user's, so only the framing is
			// (re-)applied. The extent is a property of this window, never of the file.
			FrameCamera(v_camera.get());
			return;
		}
	}

	// No id, or an id this pool does not know. Mint one at the default framing.
	v_camera = pool.Load<Camera>();
	v_camUid = v_camera->GetObjectID();
	// Registered under its own reserved name, never the bare "Camera": the
	// viewport camera is pooled and serialized, so on load it would otherwise
	// claim "Camera" from the document's own camera (which then reopened as
	// "Camera.001"). A project written before this fix keeps the old label.
	v_camera->o_name = pool.SetName(v_camera->GetObjectID(), "ViewportCamera");
	v_camera->SetPosition(kDefaultEye);
	v_camera->SetTarPos(kDefaultTarget);

	// Framed here rather than waiting for the next SyncCamera(): a fresh Camera is
	// 1x1, and a projection against that is squashed rather than merely unframed.
	FrameCamera(v_camera.get());

	NEURUS_LOG("[Viewport] Viewport camera created (UID " << v_camUid << ")");
}

void EditorViewport::ForgetCamera()
{
	v_camera.reset();
	v_camUid = 0;

	// The published pointer went with it. Leaving it would hand the next frame a
	// camera the pool has already dropped.
	p_camera = nullptr;
}

void EditorViewport::SyncCamera(Scene& scene)
{
	Camera* sceneCam = scene.GetActiveCamera();

	// The activated scene camera wins; the viewport's own is the fallback. This is
	// activation, not selection - selecting a camera leaves the view alone.
	p_camera = sceneCam ? static_cast<const Camera*>(sceneCam)
	                    : static_cast<const Camera*>(v_camera.get());

	// Both candidates, not just the chosen one: activation can switch the view with
	// no resize in between, and a camera that missed one would present a stretched
	// image the moment it took over.
	FrameCamera(v_camera.get());
	FrameCamera(sceneCam);
}

void EditorViewport::FrameCamera(Camera* cam) const
{
	if (!cam) return;
	if (v_logical.x == 0u || v_logical.y == 0u) return; // no viewport yet (startup)

	const float w = static_cast<float>(v_logical.x);
	const float h = static_cast<float>(v_logical.y);

	// Guarded on an actual change: ChangeCamRatio() recomputes the projection, and
	// this runs twice per frame for both candidates.
	if (cam->cam_w == w && cam->cam_h == h) return;

	cam->ChangeCamRatio(w, h);
}

// ---------------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------------

void EditorViewport::HandleResize(glm::uvec2 logical, glm::uvec2 renderExtent)
{
	// A zero extent is dropped rather than stored: it is what the window reports
	// before it is first shown, and it would divide by zero in the projection.
	if (logical.x == 0u || logical.y == 0u) return;

	SetViewportSize(logical, renderExtent);

	// Immediately, not via the event queue: a resize must be in effect for the
	// frame that follows it. The activated scene camera is re-framed by the next
	// SyncCamera().
	FrameCamera(v_camera.get());

	// Constant-pixel guide geometry is a CPU computation against the viewport
	// height, so a resize changes the world-space guide even mid-gesture.
	v_gizmoDraw.MarkDirty();
}

void EditorViewport::SetViewportSize(glm::uvec2 logical, glm::uvec2 renderExtent)
{
	v_logical = logical;
	v_render  = renderExtent;
}

void EditorViewport::SetCamera(const Camera* cam)
{
	p_camera = cam;
}

void EditorViewport::SetCursor(glm::vec2 logicalPixel)
{
	v_cursor = logicalPixel;

	// Only while a gesture is live: the guide follows the cursor through a pixel
	// budget, so its world-space geometry changes even when nothing else did.
	if (v_gizmo.IsActive())
		v_gizmoDraw.MarkDirty();
}

// ---------------------------------------------------------------------------
// Overlays
// ---------------------------------------------------------------------------

void EditorViewport::RebuildOverlays(const Scene& scene)
{
	// Returns immediately when clean, which is the common case: debug objects are
	// stateful and rarely move.
	v_debugDraw.Rebuild(scene);

	// The anchor is always the *live* transform - of the gesture's target while one
	// is armed, of the selection's active object otherwise. Passing the transform in
	// keeps the builder a pure geometry flattener.
	TransformSnapshot anchor{};
	const bool hasAnchor = GizmoAnchor(scene, anchor);
	v_gizmoDraw.Rebuild(v_gizmo, *this, hasAnchor ? &anchor : nullptr);
}

void EditorViewport::MarkOverlaysDirty()
{
	v_debugDraw.MarkDirty();
	v_gizmoDraw.MarkDirty();
}

void EditorViewport::MarkGizmoDirty()
{
	v_gizmoDraw.MarkDirty();
}

bool EditorViewport::GizmoAnchor(const Scene& scene, TransformSnapshot& out) const
{
	// An armed gesture names its own target by uid, which need not still be the
	// active object: the selection can change mid-gesture, and following that would
	// tear the guide off the object actually being transformed.
	const ObjectID* obj = v_gizmo.IsActive() ? scene.GetObjectID(v_gizmo.ObjectUid())
	                                         // The active object, not the whole
	                                         // selection: a gesture holds a single
	                                         // uid, so a second handle would promise
	                                         // a multi-object transform that does not
	                                         // exist.
	                                         : scene.selections.GetActiveObject();
	if (!obj)
		return false;  // nothing selected, or deleted mid-gesture (the builder then
		               // falls back to the gesture's own snapshot)

	// dynamic_cast rather than ObjectID::GetTransform(), as in ScreenPosition():
	// the virtual is non-const and hands back a void* typed as the Transform base,
	// while both lookups above are const.
	const auto* transform = dynamic_cast<const Transform3D*>(obj);
	if (!transform)
		return false;  // an object with no spatial transform has nothing to anchor on

	out.position = transform->GetPosition();
	out.rotation = transform->GetRotation();
	out.scale    = transform->GetScale();
	return true;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

float EditorViewport::DeviceRatio() const
{
	if (v_logical.x == 0u || v_render.x == 0u)
		return 1.0f;

	return static_cast<float>(v_render.x) / static_cast<float>(v_logical.x);
}

bool EditorViewport::IsValid() const
{
	return p_camera != nullptr && v_logical.x > 0u && v_logical.y > 0u;
}

glm::vec3 EditorViewport::ViewAxis() const
{
	if (!p_camera)
		return glm::vec3{0.0f};

	// A camera whose target sits on its own eye has no view direction at all. Returning
	// the zero vector rather than normalizing it keeps the NaN out of every caller.
	const glm::vec3 toTarget = p_camera->cam_tar - p_camera->GetPosition();
	const float len = glm::length(toTarget);
	return (len > kMinViewAxisLength) ? toTarget / len : glm::vec3{0.0f};
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

glm::vec2 EditorViewport::ToRenderPixels(glm::vec2 logical) const
{
	if (v_logical.x == 0u || v_logical.y == 0u)
		return logical;

	// Per-axis rather than one DeviceRatio() multiply: the two axes are equal on
	// every display seen so far, and scaling them independently costs nothing while
	// removing the assumption.
	const glm::vec2 scale = glm::vec2(v_render) / glm::vec2(v_logical);
	return logical * scale;
}

ScreenPoint EditorViewport::Project(const glm::vec3& world) const
{
	ScreenPoint out;
	if (!IsValid())
		return out;

	const glm::mat4 viewProj = p_camera->GetProjectionMatrix() * p_camera->GetViewMatrix();
	const glm::vec4 clip     = viewProj * glm::vec4(world, 1.0f);

	out.viewDepth = clip.w;

	// Behind (or on) the eye: there is no meaningful pixel. Dividing anyway would
	// mirror the point through the viewport centre and look entirely plausible.
	if (clip.w < kMinW)
		return out;

	out.pixel   = PixelOf(clip, glm::vec2(v_logical));
	out.visible = true;
	return out;
}

ScreenRay EditorViewport::RayThrough(glm::vec2 logicalPixel) const
{
	ScreenRay out;
	if (!IsValid())
		return out;

	const glm::mat4 viewProj = p_camera->GetProjectionMatrix() * p_camera->GetViewMatrix();
	const glm::mat4 inverseVP = glm::inverse(viewProj);

	// Pixel -> NDC is the exact inverse of PixelOf(), so this round-trips Project()
	// including its Y convention. Under GLM_FORCE_DEPTH_ZERO_TO_ONE the near plane
	// is NDC z = 0 and the far plane z = 1.
	const glm::vec2 ndc = (logicalPixel / glm::vec2(v_logical)) * 2.0f - 1.0f;

	const glm::vec4 nearH = inverseVP * glm::vec4(ndc.x, ndc.y, 0.0f, 1.0f);
	const glm::vec4 farH  = inverseVP * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);

	if (std::abs(nearH.w) < kMinW || std::abs(farH.w) < kMinW)
		return out;

	const glm::vec3 nearP = glm::vec3(nearH) / nearH.w;
	const glm::vec3 farP  = glm::vec3(farH) / farH.w;

	const glm::vec3 delta = farP - nearP;
	const float     len   = glm::length(delta);
	if (len < kMinW)
		return out;

	out.origin    = nearP;
	out.direction = delta / len;
	out.valid     = true;
	return out;
}

// ---------------------------------------------------------------------------
// ProjectBox
// ---------------------------------------------------------------------------

namespace
{

/// @brief Running min/max over projected pixels, with the guard-band cap applied.
struct PixelAccumulator
{
	glm::vec2 lo{0.0f};
	glm::vec2 hi{0.0f};
	glm::vec2 bandLo{0.0f};
	glm::vec2 bandHi{0.0f};
	bool      any     = false;
	bool      clamped = false;

	explicit PixelAccumulator(const glm::vec2& size)
	{
		const float band = EditorViewport::kGuardBandViewports;
		bandLo = -band * size;
		bandHi = (1.0f + band) * size;
	}

	void Add(glm::vec2 pixel)
	{
		const glm::vec2 capped = glm::clamp(pixel, bandLo, bandHi);
		if (capped != pixel)
			clamped = true;

		if (!any)
		{
			lo  = capped;
			hi  = capped;
			any = true;
			return;
		}

		lo = glm::min(lo, capped);
		hi = glm::max(hi, capped);
	}
};

/**
 * @brief Adds the near-plane crossing of every edge with one endpoint in front.
 *
 * This must happen in homogeneous clip space, before the perspective divide: a
 * corner with w <= 0 has already been mirrored through the origin by the divide,
 * so a post-divide clamp would produce a plausible but wrong rect. w is affine in
 * world position, so the crossing parameter below is exact.
 */
void ClipBoxEdges(const std::array<glm::vec4, 8>& clip,
                  float nearW,
                  const glm::vec2& size,
                  PixelAccumulator& acc)
{
	for (const auto& e : kBoxEdges)
	{
		const glm::vec4& a = clip[e[0]];
		const glm::vec4& b = clip[e[1]];

		const bool aFront = a.w > nearW;
		const bool bFront = b.w > nearW;
		if (aFront == bFront)
			continue;

		const float denom = b.w - a.w;
		if (std::abs(denom) < EditorViewport::kMinW)
			continue;

		const float t = (nearW - a.w) / denom;
		acc.Add(PixelOf(a + (b - a) * t, size));
	}
}

} // namespace

ScreenRect EditorViewport::ProjectBox(const glm::mat4& model,
                                     const glm::vec3& localMin,
                                     const glm::vec3& localMax) const
{
	ScreenRect out;
	if (!IsValid())
		return out;

	const glm::mat4 viewProj = p_camera->GetProjectionMatrix() * p_camera->GetViewMatrix();
	const glm::mat4 mvp      = viewProj * model;
	const glm::vec2 size     = glm::vec2(v_logical);

	// Clip against the near plane, not against w = 0: a corner closer than the near
	// plane is not visible anyway, and keeping w >= cam_near bounds how far a
	// straddling box can project.
	const float nearW = std::max(p_camera->cam_near, kMinW);

	const std::array<glm::vec3, 8> corners = BoxCorners(localMin, localMax);
	std::array<glm::vec4, 8>       clip{};
	for (int i = 0; i < 8; ++i)
		clip[i] = mvp * glm::vec4(corners[i], 1.0f);

	PixelAccumulator acc(size);
	for (const glm::vec4& c : clip)
	{
		if (c.w > nearW)
			acc.Add(PixelOf(c, size));
	}

	ClipBoxEdges(clip, nearW, size, acc);

	// No corner in front and no edge crossing: the box is wholly behind the eye.
	if (!acc.any)
		return out;

	out.min     = acc.lo;
	out.max     = acc.hi;
	out.valid   = true;
	out.clamped = acc.clamped;
	return out;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

float PixelsPerWorldUnit(const EditorViewport& vp, float viewDepth)
{
	if (!vp.IsValid())
		return 0.0f;

	const glm::mat4 proj = vp.GetCamera()->GetProjectionMatrix();

	// proj[1][1] carries the FOV term and no aspect term, which is why this is
	// aspect-independent; abs() because the projection negates it for Vulkan NDC.
	const float halfHeight = static_cast<float>(vp.Size().y) * 0.5f;
	return std::abs(proj[1][1]) * halfHeight / std::max(viewDepth, EditorViewport::kMinW);
}

ScreenRect ProjectBounds(const EditorViewport& vp,
                         const glm::vec3& worldMin,
                         const glm::vec3& worldMax)
{
	return vp.ProjectBox(glm::mat4(1.0f), worldMin, worldMax);
}

ScreenPoint ScreenPosition(const EditorViewport& vp, const ObjectID& object)
{
	// dynamic_cast rather than ObjectID::GetTransform(): that virtual is non-const
	// and hands back a void* typed as the Transform base. An object without a
	// Transform3D yields an invisible point, never a silent (0,0,0).
	const auto* transform = dynamic_cast<const Transform3D*>(&object);
	if (transform == nullptr)
		return ScreenPoint{};

	return vp.Project(transform->GetPosition());
}

} // namespace neurus
