#pragma once

namespace neurus {

/**
 * @brief Camera navigation events carry the camera's integer UID (not a raw
 * pointer - the camera could be deleted mid-drag; a UID stays stable). The
 * controller resolves the id against the current scene at dispatch time.
 */

struct CameraZoomEvent {
	int camId;
	float scroll_dir;
};

struct CameraRotateEvent {
	int camId;
	float mouse_delta_x, mouse_delta_y;
};

struct CameraPushEvent {
	int camId;
	float mouse_delta_x, mouse_delta_y;
};

struct CameraSlideEvent {
	int camId;
	float mouse_delta_x, mouse_delta_y;
};

struct CameraSpinEvent {
	int camId;
	float mouse_delta_x, mouse_delta_y;
};

struct CameraResizeEvent {
	int camId;
	int width; int height;
};

/**
 * @brief Emitted when a continuous camera drag gesture begins (mouse press).
 *
 * The controller captures the camera's "before" pose and mutates live during the
 * drag without recording, so the gesture collapses to one undo entry committed
 * on CameraDragEnd.
 */
struct CameraDragBegin {
	int camId;
};

/**
 * @brief Emitted when a continuous camera drag gesture ends (mouse release).
 *
 * The controller records a single transform op spanning the captured pose
 * through the current pose, if the pose actually changed.
 */
struct CameraDragEnd {
	int camId;
};

} // namespace neurus
