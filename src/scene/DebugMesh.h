/**
 * @file DebugMesh.h
 * @brief Debug wireframe-mesh primitive for viewport visualization.
 *
 * Draws pooled mesh geometry as a wireframe overlay instead of a shaded
 * surface. Reference geometry comes from pooled MeshData; appearance is
 * fully described by color, opacity and x-ray.
 *
 * @note Line width is deliberately absent: the wireframe is rasterized with
 *       polygonMode = eLine, whose width is fixed at 1 px where the wideLines
 *       feature is unavailable (notably MoltenVK/Metal, which clamps
 *       lineWidthRange to [1,1]).
 */

#pragma once

#include <memory>

#include <cereal/types/base_class.hpp>
#include <glm/glm.hpp>

#include "core/Serialize.h"
#include "scene/ObjectID.h"
#include "Transform.h"

namespace neurus
{

class MeshData;

/**
 * @brief Debug wireframe overlay drawn from pooled mesh geometry.
 *
 * Has no material and no shader of its own; the serialized geometry
 * reference is o_meshDataId.
 *
 * Object type: GO_DM
 */
class DebugMesh : public ObjectID, public Transform3D
{
public:
	/**
	 * @brief Pooled geometry drawn as wireframe (null = nothing to draw).
	 *
	 * Public so it can be re-wired from o_meshDataId after deserialization.
	 */
	std::shared_ptr<MeshData> o_mesh;

	/// @brief Pooled MeshData UID (0 = none); the serialized form of o_mesh.
	int o_meshDataId = 0;

	/** @brief Constructs an empty DebugMesh (no geometry). */
	DebugMesh();

	/**
	 * @brief Constructs a DebugMesh referencing pooled geometry.
	 * @param meshData Shared MeshData (pooled resource) to draw as wireframe.
	 */
	explicit DebugMesh(std::shared_ptr<MeshData> meshData);

	~DebugMesh() override = default;

	/**
	 * @brief Cereal serialization for debug meshes.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note Stores o_meshDataId, not o_mesh.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<ObjectID>(this),
		   cereal::make_nvp("transform", cereal::base_class<Transform3D>(this)));
		archive::Optional(ar, cereal::make_nvp("m_meshDataId", o_meshDataId),
		                  cereal::make_nvp("m_color", o_color),
		                  cereal::make_nvp("m_opacity", o_opacity),
		                  cereal::make_nvp("m_xray", o_xray));
	}

	// Non-copyable and non-movable (UID semantics): the UID base deletes both
	// copy operations, which implicitly deletes the moves.
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
