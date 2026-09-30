/**
 * @file CameraController.cpp
 * @brief Event-driven camera navigation: orbit (rotate), dolly (push), pan (slide), zoom.
 *
 * Stateless — all handlers are free functions in an anonymous namespace.
 * Each handler receives a discrete camera event, extracts the camera
 * pointer and delta values, and applies the corresponding transform math.
 * Speed control is external — Editor scales deltas before enqueuing events.
 */

#include "editor/controllers/CameraController.h"
#include "editor/events/CameraEvents.h"
#include "editor/events/EditorEvents.h"
#include "editor/events/EventBus.h"
#include "editor/operations/IOperationSink.h"
#include "editor/operations/SceneOperations.h"
#include "scene/Camera.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

// ---------------------------------------------------------------------------
// Sensitivity constants
// ---------------------------------------------------------------------------

/** @brief Base sensitivity for orbit rotation (radians per pixel). */
constexpr float kOrbitSensitivity = 0.005f;

/** @brief Base sensitivity for pan translation (world units per pixel). */
constexpr float kPanSensitivity = 0.01f;

/** @brief Base sensitivity for zoom (dolly factor per scroll notch). */
constexpr float kZoomSensitivity = 1.0f;

/** @brief Base sensitivity for dolly translation (world units per pixel). */
constexpr float kDollySensitivity = 0.05f;

/**
 * @brief Resolves a camera event's camId to its live Camera (via the resource pool).
 *
 * Looks the id up in the pool rather than in `scene->cam_list`, and that is the
 * whole point: this controller manipulates *whichever* camera the UID names and
 * deliberately does not know which one the viewport is currently looking through.
 * The editor's own viewport camera is pooled like any other camera but is not scene
 * content, so it is absent from `cam_list` — a scene-scoped lookup would silently
 * refuse to navigate it. Every camera is created through
 * `ResourceManager::Load<Camera>()`, so the pool resolves scene and editor cameras
 * alike.
 *
 * @return Non-owning Camera*, or nullptr if the id is stale or names another type.
 */
neurus::Camera* ResolveCamera(const neurus::ControllerContext& ctx, int camId)
{
	return ctx.resources.Get<neurus::Camera>(camId).get();
}

// ---------------------------------------------------------------------------
// Event notification
// ---------------------------------------------------------------------------

/**
 * @brief Notifies downstream systems of a camera transform change.
 * @param camera Camera that was modified.
 * @note Stub — will enqueue a CameraTransformChanged event in the future.
 */
void NotifyCameraChanged(const neurus::Camera& camera)
{
	(void)camera;
}

/**
 * @brief Records the coupled camera pose transition for undo/redo.
 * @tparam Op Concrete op type: CameraTransformOp (drag, non-mergeable) or
 *         CameraZoomOp (scroll, mergeable).
 * @param ops Operation sink.
 * @param camera Camera after the navigation math ran.
 * @param beforePos Camera position captured before the edit.
 * @param beforeTar Look-at target captured before the edit.
 * @note No-op if the pose did not change (degenerate/clamped moves), so the
 *       history is not polluted. The op type — not a flag — decides whether
 *       consecutive same-camera edits coalesce.
 */
template<typename Op>
void RecordCameraPose(neurus::IOperationSink& ops, const neurus::Camera& camera,
                      const glm::vec3& beforePos, const glm::vec3& beforeTar)
{
	const glm::vec3 afterPos = camera.GetPosition();
	const glm::vec3 afterTar = camera.cam_tar;
	if (afterPos == beforePos && afterTar == beforeTar) return;

	ops.Submit(std::make_unique<Op>(
		camera.GetObjectID(),
		neurus::CameraPose{ beforePos, beforeTar },
		neurus::CameraPose{ afterPos, afterTar }));
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

/**
 * @brief Handles CameraZoomEvent — moves camera toward/away from target.
 */
void OnCameraZoom(const neurus::CameraZoomEvent& e, const neurus::ControllerContext& ctx)
{
	neurus::Camera* cam = ResolveCamera(ctx, e.camId);
	if (!cam) return;
	neurus::Camera& camera = *cam;

	const glm::vec3 pos = camera.GetPosition();
	const glm::vec3& target = camera.cam_tar;

	const glm::vec3 dir = pos - target;
	const float radius = glm::length(dir);
	if (radius < 1e-6f) return;

	// Zoom factor: scroll up → closer, scroll down → farther
	const float factor = std::pow(0.8f, e.scroll_dir * kZoomSensitivity);
	const float newRadius = radius * factor;

	// Clamp to prevent going through target or flying to infinity
	constexpr float kMinRadius = 0.01f;
	constexpr float kMaxRadius = 1000.0f;
	const float clampedRadius = std::clamp(newRadius, kMinRadius, kMaxRadius);

	const glm::vec3 newDir = glm::normalize(dir) * clampedRadius;
	camera.SetPosition(target + newDir);

	NotifyCameraChanged(camera);
}

/**
 * @brief Handles CameraRotateEvent — orbits camera around target.
 */
void OnCameraRotate(const neurus::CameraRotateEvent& e, const neurus::ControllerContext& ctx)
{
	neurus::Camera* cam = ResolveCamera(ctx, e.camId);
	if (!cam) return;
	neurus::Camera& camera = *cam;

	const glm::vec3 pos = camera.GetPosition();
	const glm::vec3& target = camera.cam_tar;

	glm::vec3 dir = pos - target;
	const float radius = glm::length(dir);
	if (radius < 1e-6f) return;

	dir /= radius; // normalize

	// Compute spherical coordinates (Z-up)
	const float elevation = std::asin(dir.z);                     // [-PI/2, PI/2]
	const float azimuth   = std::atan2(dir.x, dir.y);             // [-PI, PI] — angle from +Y (forward)

	// Apply mouse delta (inverted for natural drag feel)
	const float deltaAzimuth   = e.mouse_delta_x * kOrbitSensitivity;
	const float deltaElevation = e.mouse_delta_y * kOrbitSensitivity;

	// Clamp elevation to avoid flipping at +/-89 degrees
	constexpr float kMaxElevation = glm::radians(89.0f);
	const float newElevation = std::clamp(elevation + deltaElevation, -kMaxElevation, kMaxElevation);
	const float newAzimuth   = azimuth + deltaAzimuth;

	// Convert back to Cartesian
	const float cosPhi = std::cos(newElevation);
	const glm::vec3 newDir(
		cosPhi * std::sin(newAzimuth),
		cosPhi * std::cos(newAzimuth),
		std::sin(newElevation)
	);

	camera.SetPosition(target + newDir * radius);

	NotifyCameraChanged(camera);
}

/**
 * @brief Handles CameraPushEvent — dollies camera along view direction (Ctrl+MMB).
 */
void OnCameraPush(const neurus::CameraPushEvent& e, const neurus::ControllerContext& ctx)
{
	neurus::Camera* cam = ResolveCamera(ctx, e.camId);
	if (!cam) return;
	neurus::Camera& camera = *cam;

	const glm::vec3 pos = camera.GetPosition();
	const glm::vec3& target = camera.cam_tar;

	// Forward direction: from camera position toward look-at target
	glm::vec3 dir = target - pos;
	const float len = glm::length(dir);
	if (len < 1e-6f) return;
	dir /= len;

	// Positive mouse_delta_y = move forward (toward target)
	const float dollyAmount = e.mouse_delta_y * kDollySensitivity;
	const glm::vec3 offset = dir * dollyAmount;

	camera.SetPosition(pos + offset);

	NotifyCameraChanged(camera);
}

/**
 * @brief Handles CameraSlideEvent — pans camera parallel to view plane (Shift+MMB).
 */
void OnCameraSlide(const neurus::CameraSlideEvent& e, const neurus::ControllerContext& ctx)
{
	neurus::Camera* cam = ResolveCamera(ctx, e.camId);
	if (!cam) return;
	neurus::Camera& camera = *cam;

	const glm::vec3 pos = camera.GetPosition();
	const glm::vec3& target = camera.cam_tar;

	// Forward: target → camera (viewing direction is camera looking at target)
	glm::vec3 forward = pos - target;
	if (glm::length(forward) < 1e-6f) return;
	forward = glm::normalize(forward);

	// Right: cross(forward, world-up)
	constexpr glm::vec3 kWorldUp(0.0f, 0.0f, 1.0f);
	glm::vec3 right = glm::cross(forward, kWorldUp);

	// Handle degenerate case: camera looking straight up/down
	if (glm::length(right) < 1e-6f)
	{
		right = glm::vec3(1.0f, 0.0f, 0.0f);
	}
	right = glm::normalize(right);

	// Up: cross(right, forward) — camera-local up (not world up)
	const glm::vec3 up = glm::cross(right, forward);

	// Compute translation: horizontal mouse = right, vertical mouse = up
	const glm::vec3 delta = e.mouse_delta_x * kPanSensitivity * right
	                        + e.mouse_delta_y * kPanSensitivity * up;

	camera.SetPosition(pos + delta);
	camera.SetTarPos(target + delta);

	NotifyCameraChanged(camera);
}

/**
 * @brief Handles CameraResizeEvent — updates camera aspect ratio on viewport resize.
 */
void OnCameraResize(const neurus::CameraResizeEvent& e, const neurus::ControllerContext& ctx)
{
	neurus::Camera* cam = ResolveCamera(ctx, e.camId);
	if (!cam) return;
	neurus::Camera& camera = *cam;
	camera.ChangeCamRatio(static_cast<float>(e.width), static_cast<float>(e.height));
	NotifyCameraChanged(camera);
}

} // anonymous namespace

namespace neurus {

// ---------------------------------------------------------------------------
// Init — subscribe to camera events
// ---------------------------------------------------------------------------

void CameraController::Init(ControllerContext& ctx)
{
	// --- Gesture boundaries: bound a continuous orbit/pan/dolly drag ---
	ctx.events.subscribe<CameraDragBegin>([this, ctx](const CameraDragBegin& e) {
		neurus::Camera* cam = ResolveCamera(ctx, e.camId);
		if (!cam) return;
		m_dragging = true;
		m_camId = e.camId;
		m_before = CameraPose{ cam->GetPosition(), cam->cam_tar };
	});
	ctx.events.subscribe<CameraDragEnd>([this, ctx](const CameraDragEnd& e) {
		if (m_dragging && m_camId == e.camId)
		{
			// One op per gesture, non-mergeable: a following separate drag on
			// the same camera stays its own undo entry.
			if (neurus::Camera* cam = ResolveCamera(ctx, e.camId))
				RecordCameraPose<CameraTransformOp>(ctx.ops, *cam, m_before.position, m_before.target);
		}
		m_dragging = false;
		m_camId = 0;
	});

	// --- Continuous drag moves: mutate live, DO NOT record (bounded above) ---
	ctx.events.subscribe<CameraRotateEvent>([ctx](const CameraRotateEvent& e) {
		OnCameraRotate(e, ctx);
		ctx.events.enqueue(RenderResetEvent{});
	});
	ctx.events.subscribe<CameraPushEvent>([ctx](const CameraPushEvent& e) {
		OnCameraPush(e, ctx);
		ctx.events.enqueue(RenderResetEvent{});
	});
	ctx.events.subscribe<CameraSlideEvent>([ctx](const CameraSlideEvent& e) {
		OnCameraSlide(e, ctx);
		ctx.events.enqueue(RenderResetEvent{});
	});

	// --- Scroll zoom: no press/release, record per-event and merge via MergeKey ---
	ctx.events.subscribe<CameraZoomEvent>([ctx](const CameraZoomEvent& e) {
		neurus::Camera* cam = ResolveCamera(ctx, e.camId);
		if (!cam) return;
		const glm::vec3 bp = cam->GetPosition();
		const glm::vec3 bt = cam->cam_tar;
		OnCameraZoom(e, ctx);
		RecordCameraPose<CameraZoomOp>(ctx.ops, *cam, bp, bt);
		ctx.events.enqueue(RenderResetEvent{});
	});

	// Resize is viewport-driven (aspect ratio), not a user edit — not recorded.
	ctx.events.subscribe<CameraResizeEvent>([ctx](const CameraResizeEvent& e) {
		OnCameraResize(e, ctx);
		ctx.events.enqueue(RenderResetEvent{});
	});
}

} // namespace neurus
