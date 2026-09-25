/**
 * @file DebugMesh.h
 * @brief Debug wireframe-mesh primitive for viewport visualization.
 *
 * DebugMesh is a first-class scene object that draws pooled geometry as a
 * wireframe overlay instead of a shaded surface. It completes the debug-object
 * family alongside DebugLine (segments) and DebugPoints (sprites), so a
 * wireframe visualization can be authored, selected, transformed, serialized
 * and undone like any other scene object.
 *
 * Architecture:
 * - Inherits ObjectID for scene graph identity and type discrimination (GO_DM)
 * - Inherits Transform3D for world-space placement
 * - References pooled MeshData exactly like Mesh does, so it reuses the
 *   existing MeshGPU upload path (SceneObjectGpuUploadRequested) and the
 *   ResourceComponent re-wiring on project load
 * - No GPU resources and no shader of its own: the renderer draws every
 *   DebugMesh through DebugPass's single wireframe pipeline
 *
 * @note Unlike Mesh, a DebugMesh has no Material and no per-object Shader —
 *       its appearance is fully described by color/opacity/xray.
 * @note Line width is deliberately absent. The wireframe is rasterized with
 *       polygonMode = eLine, whose width is fixed at 1 px wherever the
 *       wideLines feature is unavailable (notably MoltenVK/Metal, which clamps
 *       lineWidthRange to [1,1]). Use DebugLine when thickness is required —
 *       it expands segments to screen-space quads and honours o_width.
 */

#pragma once

#include <memory>

#include <cereal/types/base_class.hpp>
#include <glm/glm.hpp>

#include "scene/ObjectID.h"
#include "Transform.h"

namespace neurus
{

class MeshData;

/**
 * @brief Debug wireframe overlay drawn from pooled mesh geometry.
 *
 * Object type: GO_DM
 *
 * Usage:
 * @code
 *   auto dm = resources.Load<DebugMesh>(meshData);
 *   dm->SetColor({0, 1, 0, 1});   // green wireframe
 *   dm->SetXRay(true);            // ignore depth, draw on top
 *   scene.UseDebugMesh(dm);
 * @endcode
 */
class DebugMesh : public ObjectID, public Transform3D
{
public:
	/**
	 * @brief Pooled geometry drawn as wireframe (null = nothing to draw).
	 *
	 * Public, and paired with o_meshDataId below, to match Mesh: the loader
	 * (ResourceComponent) re-wires the pointer from the ID after
	 * deserialization, and the Editor reads it to request a MeshGPU upload.
	 */
	std::shared_ptr<MeshData> o_mesh;

	/// @brief Pooled MeshData UID (0 = none); the serialized form of o_mesh.
	int o_meshDataId = 0;

	/**
	 * @brief Constructs an empty DebugMesh (no geometry).
	 *
	 * Defaults: color white, opacity 1.0, x-ray off, o_type GO_DM.
	 */
	DebugMesh();

	/**
	 * @brief Constructs a DebugMesh referencing pooled geometry.
	 * @param meshData Shared MeshData (pooled resource) to draw as wireframe.
	 * @note Sets both o_mesh and o_meshDataId, mirroring Mesh(std::shared_ptr<MeshData>).
	 */
	explicit DebugMesh(std::shared_ptr<MeshData> meshData);

	~DebugMesh() override = default;

	/**
	 * @brief Cereal serialization for debug meshes.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note Stores o_meshDataId, not o_mesh — the pointer is re-wired on load.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<ObjectID>(this),
		   cereal::make_nvp("transform", cereal::base_class<Transform3D>(this)),
		   cereal::make_nvp("m_meshDataId", o_meshDataId),
		   cereal::make_nvp("m_color", o_color),
		   cereal::make_nvp("m_opacity", o_opacity),
		   cereal::make_nvp("m_xray", o_xray));
	}

	// Neither copyable nor movable: UID deletes both copy operations, which
	// implicitly deletes the moves too, so defaulting them here would only earn a
	// -Wdefaulted-function-deleted warning for operations that cannot exist.
	// Scene objects are always held by Resource<T>, so nothing needs them.
	// (DebugLine and DebugPoints still default theirs and do warn; Mesh is the
	// pattern followed here.)
	DebugMesh(const DebugMesh&) = delete;
	DebugMesh& operator=(const DebugMesh&) = delete;
	DebugMesh(DebugMesh&&) = delete;
	DebugMesh& operator=(DebugMesh&&) = delete;

	// -----------------------------------------------------------------------
	// Geometry
	// -----------------------------------------------------------------------

	/**
	 * @brief Points this DebugMesh at pooled geometry.
	 * @param meshData Shared MeshData to draw, or null to clear the reference.
	 * @note Keeps o_meshDataId in sync so the change survives a save/load.
	 */
	void SetMeshData(std::shared_ptr<MeshData> meshData);

	/** @brief Returns true when this object references geometry to draw. */
	bool HasGeometry() const { return o_mesh != nullptr; }

	// -----------------------------------------------------------------------
	// Properties
	// -----------------------------------------------------------------------

	/** @brief Sets the wireframe color. */
	void SetColor(const glm::vec4& color) { o_color = color; }
	/** @brief Returns the wireframe color. */
	const glm::vec4& GetColor() const { return o_color; }

	/** @brief Sets the opacity (0.0 = fully transparent, 1.0 = fully opaque). */
	void SetOpacity(float opacity) { o_opacity = opacity; }
	/** @brief Returns the opacity value. */
	float GetOpacity() const { return o_opacity; }

	/** @brief Enables x-ray mode: skip the depth test so the wireframe is never occluded. */
	void SetXRay(bool xray) { o_xray = xray; }
	/** @brief Returns whether x-ray mode is enabled. */
	bool GetXRay() const { return o_xray; }

	/** @brief Returns this object's Transform component. */
	void* GetTransform() override { return static_cast<Transform*>(this); }

private:
	glm::vec4 o_color{1.0f, 1.0f, 1.0f, 1.0f}; ///< Wireframe RGBA color.
	float o_opacity{1.0f};                      ///< Opacity (0-1).
	bool o_xray{false};                         ///< Draw on top, ignoring depth.
};

} // namespace neurus
