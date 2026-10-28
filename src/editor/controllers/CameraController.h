/**
 * @file CameraController.h
 * @brief Camera manipulation controller - event-driven, no per-frame polling.
 *
 * Translates discrete camera events (zoom, rotate, push, slide) into camera
 * transform updates. A continuous drag is bounded by a controller-owned gesture:
 * CameraDragBegin captures the before pose, the drag handlers mutate live
 * without recording, and CameraDragEnd submits a single non-mergeable op. Scroll
 * zoom has no press/release, so it records a mergeable per-camera op instead.
 *
 * @note Does not own the camera; each event carries a UID, resolved by the
 *       handler against the current scene.
 * @note Delta scaling is external: the emitting handler normalizes each delta.
 */
#pragma once

#include "editor/controllers/Controllers.h"
#include "editor/events/EventBus.h"
#include "editor/operations/SceneOperations.h"

namespace neurus {

class Camera;

// ---------------------------------------------------------------------------
// CameraController
// ---------------------------------------------------------------------------

/**
 * @brief Event-driven camera manipulation controller.
 *
 * Holds a small gesture state (the pose captured at drag start + the target
 * camera) so a continuous orbit/pan/dolly collapses to one undo entry. Handler
 * math lives in free functions in the .cpp; the gesture bookkeeping lives here.
 */
class CameraController : public Controllers
{
public:
	CameraController() = default;

	/**
	 * @brief Subscribes to camera events on the given context.
	 *
	 * Registers handlers for the drag gesture (begin/end), the continuous drag
	 * moves (rotate/push/slide), scroll zoom, and resize.
	 *
	 * @param ctx Controller context (events, resources, ops, scene).
	 */
	void Init(ControllerContext& ctx) override;

private:
	bool m_dragging = false;   ///< True between CameraDragBegin and CameraDragEnd.
	int m_camId = 0;           ///< Camera UID captured at drag start.
	CameraPose m_before{};     ///< Pose captured at drag start (undo endpoint).
};

} // namespace neurus
