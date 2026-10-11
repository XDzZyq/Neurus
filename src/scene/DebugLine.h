/**
 * @file DebugLine.h
 * @brief Debug line primitive for viewport visualization.
 *
 * Stores line segment endpoints together with their rendering properties
 * (color, width, opacity, stipple). Vertices are interpreted in pairs
 * (start, end).
 */

#pragma once

#include <vector>

#include <cereal/types/base_class.hpp>
#include <cereal/types/vector.hpp>

#include "core/Serialize.h"
#include "scene/ObjectID.h"
#include "Transform.h"

namespace neurus
{

/**
 * @brief Debug line primitive for viewport wireframe visualization.
 *
 * Stores line segment vertices with their rendering attributes (color,
 * width, opacity, stipple). The line style is either solid or stipple
 * (dashed).
 *
 * Object type: GO_DL
 */
class DebugLine : public ObjectID, public Transform3D
{
public:
	/** @brief Constructs a DebugLine with default properties. */
	DebugLine();

	/** @brief Destroys the DebugLine. */
	~DebugLine() override = default;

	/**
	 * @brief Cereal serialization for debug lines.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<ObjectID>(this),
		   cereal::make_nvp("transform", cereal::base_class<Transform3D>(this)));
		archive::Optional(ar, cereal::make_nvp("m_color", o_color),
		                  cereal::make_nvp("m_width", o_width),
		                  cereal::make_nvp("m_opacity", o_opacity),
		                  cereal::make_nvp("m_stipple", o_stipple),
		                  cereal::make_nvp("m_vertices", o_vertices),
		                  cereal::make_nvp("m_xray", o_xray));
	}

	// Non-copyable and non-movable (UID semantics): ObjectID's UID base deletes
	// both, so defaulting them here would only implicitly delete and warn.
	DebugLine(const DebugLine&) = delete;
	DebugLine& operator=(const DebugLine&) = delete;
	DebugLine(DebugLine&&) = delete;
	DebugLine& operator=(DebugLine&&) = delete;

	// -----------------------------------------------------------------------
	// Vertex management
	// -----------------------------------------------------------------------

	/**
	 * @brief Adds a single line segment defined by two endpoints.
	 * @param start Start position of the segment.
	 * @param end End position of the segment.
	 */
	void PushDebugLine(const glm::vec3& start, const glm::vec3& end);

	/**
	 * @brief Appends multiple vertices as line segment endpoints.
	 * @param vertices Vector of vertex positions.
	 * @note Vertices are interpreted in pairs (start0,end0, start1,end1,...).
	 *       An odd number of vertices is allowed; the last vertex forms
	 *       an incomplete segment.
	 */
	void PushDebugLines(const std::vector<glm::vec3>& vertices);

	/**
	 * @brief Removes all stored vertices.
	 */
	void ClearVertices();

	/**
	 * @brief Replaces the whole vertex list.
	 * @param vertices New vertex positions, interpreted in pairs like PushDebugLines.
	 */
	void SetVertices(std::vector<glm::vec3> vertices) { o_vertices = std::move(vertices); }

	/** @brief Returns the number of stored vertex positions. */
	int GetVertexCount() const { return static_cast<int>(o_vertices.size()); }

	/**
	 * @brief Returns a const reference to the vertex storage.
	 * @return Const reference to the vector of local-space positions.
	 */
	const std::vector<glm::vec3>& GetVertices() const { return o_vertices; }

	// -----------------------------------------------------------------------
	// Properties
	// -----------------------------------------------------------------------

	/** @brief Sets the line color. */
	void SetColor(const glm::vec4& color) { o_color = color; }
	/** @brief Returns the line color. */
	const glm::vec4& GetColor() const { return o_color; }

	/** @brief Sets the line width in pixels. */
	void SetWidth(float width) { o_width = width; }
	/** @brief Returns the line width in pixels. */
	float GetWidth() const { return o_width; }

	/** @brief Sets the opacity (0.0 = fully transparent, 1.0 = fully opaque). */
	void SetOpacity(float opacity) { o_opacity = opacity; }
	/** @brief Returns the opacity value. */
	float GetOpacity() const { return o_opacity; }

	/** @brief Enables or disables stipple (dashed) rendering. */
	void SetStipple(bool stipple) { o_stipple = stipple; }
	/** @brief Returns whether stipple is enabled. */
	bool GetStipple() const { return o_stipple; }

	/** @brief Enables x-ray mode: skip the depth test so the lines are never occluded. */
	void SetXRay(bool xray) { o_xray = xray; }
	/** @brief Returns whether x-ray mode is enabled. */
	bool GetXRay() const { return o_xray; }

	/**
	 * @brief Returns this object's Transform component.
	 *
	 * Vertices are stored in the transform's local space, so the whole line
	 * moves, rotates and scales as one.
	 */
	void* GetTransform() override { return static_cast<Transform*>(this); }

private:
	glm::vec4 o_color{1.0f, 1.0f, 1.0f, 1.0f}; ///< Line RGBA color.
	float o_width{1.0f};                         ///< Line width in pixels.
	float o_opacity{1.0f};                       ///< Opacity (0-1).
	bool o_stipple{false};                       ///< Dashed line flag.
	bool o_xray{false};                          ///< Draw on top, ignoring depth.

	std::vector<glm::vec3> o_vertices;           ///< Line segment vertex positions.
};

} // namespace neurus

