/**
 * @file RenderFrameSnapshot.h
 * @brief Owned, deep copy of everything the render layer reads for one frame.
 *
 * RenderContext publishes the frame as borrowed pointers - `scene`, `camera`,
 * `config`, the two overlay lists - which is only safe while the Editor and the
 * renderer share a thread. RenderFrameSnapshot is the same frame with the
 * borrowing taken out: every value is copied out of the Scene while the Editor
 * still owns it, so a reader may keep it for as long as it likes while the
 * Editor edits on.
 *
 * Two rules make that true, and both are load-bearing:
 *
 * 1. **No Scene pointers.** GPU resources travel as the stable UID the renderer
 *    already resolves them by (MeshGPU, LightGPU, EnvironmentGPU and the
 *    pipeline cache are all keyed by object id), never as a pointer into a pool
 *    the Editor may re-key or free.
 * 2. **No lazy reads.** Anything a pass would have asked the Scene for - a model
 *    matrix, a light direction, a camera matrix - is already computed here.
 *    Nothing in this struct dereferences anything.
 *
 * Draw order is UID order rather than the Scene's `unordered_map` order: the
 * shadow-intensity layer and descriptor-slot assignments downstream are
 * index-based, so an identical scene must produce an identically ordered frame.
 *
 * Vulkan-free, by construction, and Editor-free: one-way state from the Editor
 * to the render layer, carrying no Vulkan type and no Editor header.
 *
 * @note Visibility flags (`is_viewport` / `is_rendered`) are carried rather than
 *       applied: each pass filters differently (the sun shadow path draws meshes
 *       the G-buffer skips), so the filter stays with the pass.
 */

#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "core/RenderConfig.h"
#include "render/overlay/DebugDrawList.h"
#include "render/overlay/GizmoDrawList.h"
#include "scene/Light.h"

namespace neurus
{

/**
 * @brief One resolved mesh draw: where it is, and which GPU resources it names.
 *
 * Replaces `Scene::mesh_list`'s `(id, Mesh*)` pair. The transforms are already
 * computed and the resource references are ids, so the record stays valid after
 * the Mesh it came from is moved or destroyed.
 */
struct MeshInstanceSnapshot
{
	/// @brief Scene object UID - the key for MeshGPU and the per-mesh pipeline.
	int objectId = 0;

	/// @brief Local-to-world transform (Mesh::GetModelMatrix()).
	glm::mat4 model{1.0f};

	/// @brief Inverse-transpose of model's upper-left 3x3 (Mesh::GetNormalMatrix()).
	glm::mat3 normalMatrix{1.0f};

	/**
	 * @brief UID of the mesh's own shader, or 0 when it has none.
	 *
	 * Zero is what selects the pass's default pipeline; a non-zero id means the
	 * mesh asked for a per-mesh pipeline, cached under `objectId` + `shaderVersion`.
	 */
	int shaderObjectId = 0;

	/// @brief Shader version the per-mesh pipeline was compiled from (0 = none).
	int shaderVersion = 0;

	/// @brief False when the mesh carries no geometry; such a mesh is not drawn.
	bool hasGeometry = false;

	/// @brief ObjectID::is_viewport at snapshot time.
	bool visibleInViewport = true;

	/// @brief ObjectID::is_rendered at snapshot time.
	bool rendered = true;

	/** @brief Value equality - the detachment test compares whole frames. */
	bool operator==(const MeshInstanceSnapshot&) const = default;
};

/**
 * @brief One light's frame state: its key, its pose, and its parameters.
 *
 * Replaces `Scene::light_list`'s `(uid, Light*)` pair. `direction` is stored as
 * Light::GetDirection() returns it (unit length already, but consumers that
 * depend on it normalise, as they do today).
 */
struct LightSnapshot
{
	/// @brief Scene object UID - the key for LightGPU and its shadow-intensity layer.
	int objectId = 0;

	/// @brief Which light this is; every shadow path branches on it.
	LightType type = LightType::NONELIGHT;

	/// @brief World position (shadow cubemap origin, lighting push constants).
	glm::vec3 position{0.0f};

	/// @brief Forward direction (sun shadow view matrix, spot cone axis).
	glm::vec3 direction{0.0f, 1.0f, 0.0f};

	/// @brief RGB colour multiplier, linear space.
	glm::vec3 color{1.0f};

	/// @brief Luminous intensity.
	float power = 10.0f;

	/// @brief Physical radius driving soft-shadow penumbra width.
	float radius = 0.01f;

	/// @brief Inner cone cosine (spot lights).
	float spotCutoff = 0.9f;

	/// @brief Outer cone cosine (spot lights).
	float spotOuterCutoff = 0.8f;

	/// @brief Width/height of the rectangle (area lights).
	float areaRatio = 1.0f;

	/// @brief Light::use_shadow - false skips every shadow path for this light.
	bool useShadow = true;

	/// @brief ObjectID::is_viewport at snapshot time.
	bool visibleInViewport = true;

	/// @brief ObjectID::is_rendered at snapshot time.
	bool rendered = true;

	/** @brief Value equality - the detachment test compares whole frames. */
	bool operator==(const LightSnapshot&) const = default;
};

/**
 * @brief The viewing camera, as matrices and scalars rather than as an object.
 *
 * The matrices are copied rather than recomputed from the scalars on purpose:
 * re-deriving them downstream is how a pass ends up disagreeing with the
 * CameraGPU the vertex stage used.
 */
struct CameraSnapshot
{
	/// @brief View matrix (Camera::GetViewMatrix()).
	glm::mat4 view{1.0f};

	/// @brief Projection matrix, Y already flipped for Vulkan NDC.
	glm::mat4 projection{1.0f};

	/// @brief Eye position in world space.
	glm::vec3 position{0.0f};

	/// @brief Look-at target; the sun shadow ortho centres on it.
	glm::vec3 target{0.0f};

	/// @brief Viewport width in pixels, as the camera knows it.
	float width = 1.0f;

	/// @brief Viewport height in pixels, as the camera knows it.
	float height = 1.0f;

	/// @brief Perspective field of view in degrees.
	float fovDegrees = 60.0f;

	/// @brief Near clipping plane distance.
	float nearPlane = 0.1f;

	/// @brief Far clipping plane distance.
	float farPlane = 100.0f;

	/** @brief Value equality - the detachment test compares whole frames. */
	bool operator==(const CameraSnapshot&) const = default;
};

/**
 * @brief Everything the render layer reads for one frame, owned outright.
 *
 * Produced by Editor::BuildSnapshot() and consumed through
 * RenderContext::snapshot. The frame half (extent, frame index, jitter,
 * iteration) is deliberately absent: it is renderer-owned and stays in
 * RenderContext, stamped per frame by recordFrame().
 */
struct RenderFrameSnapshot
{
	/// @brief Value copy of the Editor's render settings. Read through Settings().
	RenderConfig config{};

	/// @brief False when the frame was built with no Scene - a legal frame.
	bool hasScene = false;

	/// @brief False when no viewing camera was published - a legal frame.
	bool hasCamera = false;

	/// @brief The camera this frame is viewed through; meaningless unless hasCamera.
	CameraSnapshot camera{};

	/// @brief Every registered mesh, in UID order. Pass-specific filtering applies.
	std::vector<MeshInstanceSnapshot> meshes;

	/// @brief Every registered light, in UID order. Pass-specific filtering applies.
	std::vector<LightSnapshot> lights;

	/// @brief False when the scene has no environment; IBL binds its placeholder.
	bool hasEnvironment = false;

	/// @brief UID of the environment supplying IBL - the key for EnvironmentGPU.
	int environmentId = 0;

	/**
	 * @brief UID of the selected object, or 0 for none.
	 *
	 * Zero is the same "no selection" the outline pass already pushes, so the
	 * value travels as-is rather than as an id plus a flag.
	 */
	int activeObjectId = 0;

	/// @brief False when debug draw is off; the debug pass draws nothing.
	bool hasDebugDraw = false;

	/// @brief This frame's debug geometry; empty unless hasDebugDraw.
	DebugDrawList debugDraw;

	/// @brief This frame's gizmo geometry; published unconditionally, often empty.
	GizmoDrawList gizmoDraw;

	// --- Null policy, spelled as RenderContext spells it ---

	/** @brief True when a Scene was published; required before reading the lists. */
	bool HasScene() const { return hasScene; }

	/** @brief True when a viewing camera was published. */
	bool HasCamera() const { return hasCamera; }

	/** @brief True when an environment was published (environmentId is then its UID). */
	bool HasEnvironment() const { return hasEnvironment; }

	/** @brief True when debug geometry was published. */
	bool HasDebugDraw() const { return hasDebugDraw; }

	/**
	 * @brief This frame's render settings.
	 *
	 * Spelled as a call for symmetry with RenderContext::Settings(), so a reader
	 * reads the same expression either way. It needs no fallback: a
	 * default-constructed RenderConfig *is* DefaultRenderConfig()'s value, which
	 * is RenderConfig's own member initializers.
	 */
	const RenderConfig& Settings() const { return config; }

	/**
	 * @brief Empties the frame but keeps the list capacity, for reuse.
	 *
	 * Lets a producer refill one snapshot instead of allocating a new one per
	 * frame, the same bargain DebugDrawList::Clear() makes.
	 */
	void Clear();
};

} // namespace neurus
