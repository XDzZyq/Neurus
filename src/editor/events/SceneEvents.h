/**
 * @file SceneEvents.h
 * @brief Scene-domain events (UI -> Editor -> SceneController).
 *
 * Events are ephemeral value structs that carry plain INTEGER ids instead of
 * pointers: a UID stays a stable identity that controllers resolve against the
 * current scene at dispatch time. Each event carries `objectUid`; the scene
 * itself is never carried, so a scene swap can never leave a stale pointer.
 */

#pragma once

#include <string>
#include <vector>

namespace neurus {

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

/** @brief Emitted when a scene object is selected (outliner click / viewport pick). */
struct ObjectSelected
{
	int objectUid = 0; ///< Selected object UID (0 = background click).
	int modifiers = 0; ///< Input::Modifiers bitmask.
};

/** @brief Emitted when a scene object is deselected. */
struct ObjectDeselected
{
	int objectUid = 0; ///< Object UID to deselect.
};

/**
 * @brief Absolute selection-set state applied to the scene.
 *
 * Emitted only when replaying a SetSelectionOp: live selection events mutate
 * the set incrementally, so the undoable operation stores the absolute list and
 * dispatches this to restore it at once.
 */
struct SelectionChanged
{
	std::vector<int> selectedUids; ///< Ordered selected object UIDs.
	int activeUid = 0;             ///< Active object UID (0 = none).
};

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

/** @brief Emitted when object visibility toggles change in the outliner. */
struct VisibilityChanged
{
	int objectUid = 0; ///< Object UID.
	bool viewportVisible = true; ///< Viewport (editor) visibility.
	bool renderVisible = true;   ///< Render (pipeline) visibility.
};

// ---------------------------------------------------------------------------
// Transform (Property Panel)
// ---------------------------------------------------------------------------

/** @brief Emitted when the position of an object is edited. */
struct PositionChanged
{
	int objectUid = 0;
	float posX = 0.0f;
	float posY = 0.0f;
	float posZ = 0.0f;
};

/** @brief Emitted when the rotation of an object is edited (degrees, Z-up Euler). */
struct RotationChanged
{
	int objectUid = 0;
	float rotX = 0.0f;
	float rotY = 0.0f;
	float rotZ = 0.0f;
};

/** @brief Emitted when the scale of an object is edited. */
struct ScaleChanged
{
	int objectUid = 0;
	float sclX = 1.0f;
	float sclY = 1.0f;
	float sclZ = 1.0f;
};

// ---------------------------------------------------------------------------
// Camera property events
// ---------------------------------------------------------------------------

struct CameraTargetChanged
{
	int objectUid = 0;
	float targetX = 0.0f;
	float targetY = 0.0f;
	float targetZ = 0.0f;
};

struct CameraFovChanged
{
	int objectUid = 0;
	float fov = 60.0f;
};

/**
 * @brief Absolute camera pose (position + look-at target) set.
 *
 * Emitted by an absolute pose write - the replay of a CameraTransformOp, or a
 * live script setting `Camera.position`. Live navigation events carry relative
 * deltas and cannot be replayed, so the op stores absolute endpoints.
 */
struct CameraPoseChanged
{
	int objectUid = 0;
	float posX = 0.0f;
	float posY = 0.0f;
	float posZ = 0.0f;
	float tarX = 0.0f;
	float tarY = 0.0f;
	float tarZ = 0.0f;
};

/**
 * @brief Absolute camera-activation set: which scene camera the viewport uses.
 *
 * `camUid` 0 means "deactivate" - a legal state in which the viewport looks
 * through its own camera. Activation is independent of selection. One event for
 * both the live toggle and the replay of SetActiveCameraOp.
 */
struct ActiveCameraChanged
{
	int camUid = 0;
};

// ---------------------------------------------------------------------------
// Mesh property events
// ---------------------------------------------------------------------------

struct MeshShadowChanged
{
	int objectUid = 0;
	bool enabled = true;
};

struct MeshMaterialChanged
{
	int objectUid = 0;
	bool enabled = true;
};

// ---------------------------------------------------------------------------
// Light property events
// ---------------------------------------------------------------------------

struct LightPowerChanged
{
	int objectUid = 0;
	float power = 10.0f;
};

struct LightColorChanged
{
	int objectUid = 0;
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
};

struct LightRadiusChanged
{
	int objectUid = 0;
	float radius = 0.05f;
};

struct LightShadowChanged
{
	int objectUid = 0;
	bool enabled = true;
};

struct LightCutoffChanged
{
	int objectUid = 0;
	float cutoff = 0.9f;
};

struct LightOuterCutoffChanged
{
	int objectUid = 0;
	float outerCutoff = 0.8f;
};

// ---------------------------------------------------------------------------
// Environment property events
// ---------------------------------------------------------------------------

struct EnvironmentIntensityChanged
{
	int objectUid = 0;
	float intensity = 1.0f;
};

struct EnvironmentRotationChanged
{
	int objectUid = 0;
	float rotation = 0.0f;
};

// ---------------------------------------------------------------------------
// Debug object property events
// ---------------------------------------------------------------------------
//
// One set of events for all three debug types: the shared knobs - color,
// opacity, x-ray - are the same property on three unrelated pools, so the
// controller resolves the UID against whichever pool holds it. A debug property
// edit is only visible after the overlay is re-flattened, which the mutation
// notification arranges.

/** @brief Debug object tint. Carries alpha: debug colors are vec4. */
struct DebugColorChanged
{
	int objectUid = 0;
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float a = 1.0f;
};

struct DebugOpacityChanged
{
	int objectUid = 0;
	float opacity = 1.0f;
};

/** @brief Draw the object through geometry (depth test off). */
struct DebugXRayChanged
{
	int objectUid = 0;
	bool xray = false;
};

struct DebugLineWidthChanged
{
	int objectUid = 0;
	float width = 1.0f;
};

struct DebugLineStippleChanged
{
	int objectUid = 0;
	bool stipple = false;
};

/** @brief Point sprite shape, as the int value of DebugPoints::PointType. */
struct DebugPointTypeChanged
{
	int objectUid = 0;
	int pointType = 0;
};

/** @brief Point size: pixels when the projection mode is screen-space, else world units. */
struct DebugPointScaleChanged
{
	int objectUid = 0;
	float scale = 8.0f;
};

/** @brief 0 = screen-space size, 1 = world-space size. */
struct DebugProjectionModeChanged
{
	int objectUid = 0;
	int projectionMode = 0;
};

/**
 * @brief Absolute replacement of a debug object's position list.
 *
 * Covers editing a coordinate, adding a row and removing a row alike, so the
 * list is always sent whole. Flattened to xyz triples because this header stays
 * glm-free. The O(n) copy is fine for hand-authored debug lists.
 */
struct DebugPositionsChanged
{
	int objectUid = 0;
	std::vector<float> xyz; ///< 3 floats per position, in object space.
};

// ---------------------------------------------------------------------------
// Scene membership (Add / Delete)
// ---------------------------------------------------------------------------

/**
 * @brief Adds an object to the scene.
 *
 * Emitted after a resource is loaded into the pool AND on undo/redo replay, so
 * replay runs the same handler as a live edit. Carries the object UID; the
 * controller fetches the pooled object, registers it, selects it, and records
 * the composite operation.
 */
struct SceneObjectAddRequested
{
	int objectUid = 0; ///< Pooled object UID.
};

/**
 * @brief Removes a BATCH of objects' scene references.
 *
 * The SINGLE removal path: emitted by the delete gesture (one event carrying
 * all selected UIDs) AND on undo/redo replay. The handler removes exactly these
 * UIDs - no selection logic, no operation recording. The pooled resources are
 * never removed from the pool.
 */
struct SceneObjectDeleteRequested
{
	std::vector<int> uids; ///< Object UIDs to remove (one or more).
};

/**
 * @brief Editor->Controller delete gesture: delete ALL selected objects (Delete key).
 *
 * FORWARD-ONLY: the recorded composite replays via SceneObjectDeleteRequested,
 * never this gesture event. The controller snapshots the selection, deselects,
 * removes every selected object, and records ONE composite operation.
 *
 * Nothing is guarded: a scene with zero cameras is legal, and deleting the
 * activated camera simply leaves a stale activation UID, which the active-camera
 * lookup reads as "none".
 */
struct ObjectDeleteRequested
{
};

} // namespace neurus
