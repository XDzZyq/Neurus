/**
 * @file SceneEvents.h
 * @brief Scene-domain events (UI -> Editor -> SceneController).
 *
 * Events are ephemeral value structs that carry plain INTEGER ids instead of
 * object/scene pointers: raw pointers can dangle after an object is deleted,
 * while a UID stays a stable identity that controllers resolve against the
 * current scene/pool at dispatch time.
 *
 * - int objectUid - the scene object being mutated (0 = none). Controllers
 *   resolve it to a live object via the ControllerContext (scene lookup for
 *   property edits, pool lookup for membership).
 * - The scene is NOT carried in events: scene-scoped handlers obtain the
 *   current Scene from the ControllerContext, so a scene swap on New/Load can
 *   never leave an event holding a stale scene pointer.
 *
 * The UI layer emits PURE INPUT INTENTS on the UI->Editor path; scene editing
 * events carry int objectUid and Editor::OnUIEvent enqueues them unchanged.
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
 * Emitted only when replaying a SetSelectionOp: live selection events
 * (ObjectSelected/ObjectDeselected) mutate the set incrementally, so the
 * undoable operation stores the absolute selected-UID list plus the active
 * UID and dispatches this to restore the whole set at once.
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
 * Emitted only when replaying a CameraTransformOp: live navigation events
 * (rotate/slide/push/zoom) carry relative deltas and cannot be replayed, so
 * the undoable operation stores the absolute endpoints and dispatches this.
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
 * `camUid` 0 means "deactivate" — a legal, fully supported state in which the
 * viewport looks through the Editor's own camera. Activation is scene state and
 * is deliberately independent of selection: the PropertyPanel keeps inspecting
 * whatever is selected, and selecting a camera never changes the view.
 *
 * One event for both the gesture (the PropertyPanel checkbox) and the replay of
 * SetActiveCameraOp, like every other scene mutation: the op stores the before
 * and after UIDs and dispatches this to re-apply either endpoint.
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
// Debug object property events (issue #22)
// ---------------------------------------------------------------------------
//
// One set of events for all three debug types (DebugLine, DebugPoints,
// DebugMesh): the shared knobs — color, opacity, x-ray — are literally the same
// property on three unrelated pools, so the controller resolves the UID against
// whichever pool holds it instead of the event naming a type. The type-specific
// knobs simply never reach an object that has no such property.
//
// Every handler ends in Mutated(), which enqueues RenderResetEvent and so marks
// DebugDrawBuilder dirty — a debug property edit is only visible after a
// reflatten.

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
 * Covers editing a coordinate, adding a row and removing a row alike — a
 * structural change could not be expressed by a per-index event, so the list is
 * always sent whole. Flattened to xyz triples because this header stays
 * glm-free like every other event in it. The O(n) copy per edit is fine for
 * hand-authored debug lists; nothing here is on a per-frame path.
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
 * Emitted by the Editor after loading a resource into the pool (mesh import,
 * camera/light add) AND by SceneObjectAddOp on undo/redo replay (the
 * originating event — replay runs the same handler as a live edit; the
 * handler's Submit is muted by Phase::Replaying). Carries the object UID; the
 * SceneController fetches the pooled object from the resource pool by UID,
 * registers it, selects it, and records the composite operation.
 */
struct SceneObjectAddRequested
{
	int objectUid = 0; ///< Pooled object UID.
};

/**
 * @brief Removes a BATCH of objects' scene references.
 *
 * The SINGLE removal path: emitted by the delete gesture (one event carrying
 * all selected UIDs) AND by SceneObjectAddOp (delete direction) on undo/redo
 * replay. The handler removes exactly these UIDs — no selection logic, no
 * operation recording (the gesture records the composite; replay is muted).
 * The pooled resources are never removed from the pool.
 */
struct SceneObjectDeleteRequested
{
	std::vector<int> uids; ///< Object UIDs to remove (one or more).
};

/**
 * @brief Editor->Controller delete gesture: delete ALL selected objects (Delete key).
 *
 * Dedicated Editor->Controller event: the Editor emits it (wrapping the pure
 * DeleteRequested input intent). FORWARD-ONLY: the recorded composite replays
 * via SceneObjectDeleteRequested, never this gesture event. The SceneController
 * snapshots the selection, deselects, removes every selected object (one batched
 * SceneObjectDeleteRequested), and records ONE composite operation
 * (selection-clear + batched delete).
 *
 * Nothing is guarded: a scene with zero cameras is legal, because the Editor
 * always has its own camera. Deleting the *activated* camera simply leaves the
 * activation UID stale, which Scene::GetActiveCamera() reads as "none" and the
 * undo re-add revalidates for free.
 */
struct ObjectDeleteRequested
{
};

} // namespace neurus
