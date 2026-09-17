/**
 * @file DebugLine.h
 * @brief Debug line primitive for viewport visualization.
 *
 * DebugLine provides CPU-side storage for debug line segments used in
 * viewport rendering. Each DebugLine instance stores a collection of
 * vertex positions (interpreted as line segment endpoints), along with
 * rendering properties (color, width, opacity, stipple, smooth).
 *
 * Architecture:
 * - Inherits ObjectID for scene graph identity and type discrimination
 * - Inherits Transform3D for world-space placement
 * - Vertex storage via std::vector<glm::vec3> - no GPU resources
 * - PushDebugLine/PushDebugLines for adding segment data
 *
 * @note No GPU resources or shaders - pure CPU data container.
 * @note Vertices are interpreted as pairs for line segments.
 */

#pragma once

#include <vector>

#include <cereal/types/base_class.hpp>
#include <cereal/types/vector.hpp>

#include "scene/ObjectID.h"
#include "Transform.h"

namespace neurus
{

/**
 * @brief Debug line primitive for viewport wireframe visualization.
 *
 * Stores line segment vertices with rendering attributes. Supports
 * stipple (dashed) and smooth (anti-aliased) line rendering modes.
 * All data is CPU-side; GPU upload is handled by the renderer layer.
 *
 * Object type: GO_DL
 *
 * Usage:
 * @code
 *   DebugLine dl;
 *   dl.PushDebugLine({0,0,0}, {1,0,0});   // single segment
 *   dl.PushDebugLines({...});                // multiple segments
 *   dl.SetColor({1,0,0,1});                 // red lines
 *   dl.SetWidth(2.0f);
 * @endcode
 */
class DebugLine : public ObjectID, public Transform3D
{
public:
	/**
	 * @brief Constructs a DebugLine with default properties.
	 *
	 * Defaults:
	 * - Color: white (1,1,1,1)
	 * - Width: 1.0
	 * - Opacity: 1.0
	 * - Stipple: false (solid)
	 * - Smooth: false
	 * - o_type: GO_DL
	 */
	DebugLine();

	/**
	 * @brief Destroys the DebugLine.
	 */
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
		   cereal::make_nvp("transform", cereal::base_class<Transform3D>(this)),
		   cereal::make_nvp("m_color", o_color),
		   cereal::make_nvp("m_width", o_width),
		   cereal::make_nvp("m_opacity", o_opacity),
		   cereal::make_nvp("m_stipple", o_stipple),
		   cereal::make_nvp("m_smooth", o_smooth),
		   cereal::make_nvp("m_vertices", o_vertices),
		   cereal::make_nvp("m_xray", o_xray));
	}

	// Non-copyable and non-movable (UID semantics): ObjectID's UID base deletes
	// both, so defaulting the move ops would only implicitly delete them and warn.
	// Scenes hold these through Resource<T> (shared_ptr), never by value.
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
	 * @brief Returns the number of stored vertex positions.
	 * @return Vertex count.
	 */
	int GetVertexCount() const { return static_cast<int>(o_vertices.size()); }

	/**
	 * @brief Returns a const reference to the vertex storage.
	 * @return Const reference to vector of vertex positions.
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

	/** @brief Enables or disables smooth (anti-aliased) rendering. */
	void SetSmooth(bool smooth) { o_smooth = smooth; }
	/** @brief Returns whether smooth rendering is enabled. */
	bool GetSmooth() const { return o_smooth; }

	/** @brief Enables x-ray mode: skip the depth test so the lines are never occluded. */
	void SetXRay(bool xray) { o_xray = xray; }
	/** @brief Returns whether x-ray mode is enabled. */
	bool GetXRay() const { return o_xray; }

private:
	glm::vec4 o_color{1.0f, 1.0f, 1.0f, 1.0f}; ///< Line RGBA color.
	float o_width{1.0f};                         ///< Line width in pixels.
	float o_opacity{1.0f};                       ///< Opacity (0-1).
	bool o_stipple{false};                       ///< Dashed line flag.
	bool o_smooth{false};                        ///< Anti-aliased line flag.
	bool o_xray{false};                          ///< Draw on top, ignoring depth.

	std::vector<glm::vec3> o_vertices;           ///< Line segment vertex positions.
};

} // namespace neurus

