/**
 * @file DebugPoints.h
 * @brief Debug point primitive for viewport visualization.
 *
 * DebugPoints provides CPU-side storage for debug point primitives used
 * in viewport rendering. Each DebugPoints instance stores a collection of
 * point positions, along with rendering properties (point type, color,
 * scale, opacity, projection mode).
 *
 * Architecture:
 * - Inherits ObjectID for scene graph identity and type discrimination
 * - Inherits Transform3D for world-space placement
 * - Point storage via std::vector<glm::vec3> - no GPU resources
 * - PointType enum for choosing point sprite shape
 * - PushDebugPoint/PushDebugPoints for adding point data
 *
 * @note No GPU resources or shaders - pure CPU data container.
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
 * @brief Debug point primitive for viewport visualization.
 *
 * Stores point positions with rendering attributes including point
 * type (SQUARE, RHOMBUS, CIR, CUBE), color, scale, opacity, and
 * projection mode. All data is CPU-side; GPU upload is handled by
 * the renderer layer.
 *
 * Object type: GO_DP
 *
 * Usage:
 * @code
 *   DebugPoints dp;
 *   dp.PushDebugPoint({0,0,0});            // single point
 *   dp.PushDebugPoints({...});              // multiple points
 *   dp.SetPointType(DebugPoints::CIR);     // circle sprites
 *   dp.SetColor({1,0,0,1});               // red color
 *   dp.SetScale(3.0f);
 * @endcode
 */
class DebugPoints : public ObjectID, public Transform3D
{
public:
	/**
	 * @brief Point sprite type enum.
	 *
	 * Determines the visual shape of each rendered point.
	 */
	enum class PointType
	{
		SQUARE,   ///< Axis-aligned square sprite.
		RHOMBUS,  ///< Diamond / rotated square sprite.
		CIR,      ///< Circular sprite.
		CUBE      ///< 3D cube sprite (volumetric).
	};

	/**
	 * @brief Constructs a DebugPoints with default properties.
	 *
	 * Defaults:
	 * - PointType: SQUARE
	 * - Color: white (1,1,1,1)
	 * - Scale: 1.0
	 * - Opacity: 1.0
	 * - ProjectionMode: 0
	 * - o_type: GO_DP
	 */
	DebugPoints();

	/**
	 * @brief Destroys the DebugPoints.
	 */
	~DebugPoints() override = default;

	/**
	 * @brief Cereal serialization for debug points.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<ObjectID>(this),
		   cereal::make_nvp("transform", cereal::base_class<Transform3D>(this)),
		   cereal::make_nvp("m_pointType", o_pointType),
		   cereal::make_nvp("m_color", o_color),
		   cereal::make_nvp("m_scale", o_scale),
		   cereal::make_nvp("m_opacity", o_opacity),
		   cereal::make_nvp("m_projectionMode", o_projectionMode),
		   cereal::make_nvp("m_points", o_points),
		   cereal::make_nvp("m_xray", o_xray));
	}

	// Non-copyable and non-movable (UID semantics): ObjectID's UID base deletes
	// both, so defaulting the move ops would only implicitly delete them and warn.
	// Scenes hold these through Resource<T> (shared_ptr), never by value.
	DebugPoints(const DebugPoints&) = delete;
	DebugPoints& operator=(const DebugPoints&) = delete;
	DebugPoints(DebugPoints&&) = delete;
	DebugPoints& operator=(DebugPoints&&) = delete;

	// -----------------------------------------------------------------------
	// Point management
	// -----------------------------------------------------------------------

	/**
	 * @brief Adds a single debug point at the given position.
	 * @param point World-space position of the point.
	 */
	void PushDebugPoint(const glm::vec3& point);

	/**
	 * @brief Appends multiple debug points.
	 * @param points Vector of world-space positions.
	 */
	void PushDebugPoints(const std::vector<glm::vec3>& points);

	/**
	 * @brief Removes all stored points.
	 */
	void ClearPoints();

	/**
	 * @brief Replaces the whole point list.
	 * @param points New world-space positions.
	 * @see DebugLine::SetVertices for why the editor sets the list absolutely.
	 */
	void SetPoints(std::vector<glm::vec3> points) { o_points = std::move(points); }

	/**
	 * @brief Returns the number of stored points.
	 * @return Point count.
	 */
	int GetPointCount() const { return static_cast<int>(o_points.size()); }

	/**
	 * @brief Returns a const reference to the point storage.
	 * @return Const reference to vector of point positions.
	 */
	const std::vector<glm::vec3>& GetPoints() const { return o_points; }

	// -----------------------------------------------------------------------
	// Properties
	// -----------------------------------------------------------------------

	/** @brief Sets the point sprite type. */
	void SetPointType(PointType type) { o_pointType = type; }
	/** @brief Returns the point sprite type. */
	PointType GetPointType() const { return o_pointType; }

	/** @brief Sets the point color. */
	void SetColor(const glm::vec4& color) { o_color = color; }
	/** @brief Returns the point color. */
	const glm::vec4& GetColor() const { return o_color; }

	/**
	 * @brief Sets the sprite diameter.
	 * @param scale Diameter in pixels when GetProjectionMode() == 0, else in world units.
	 */
	void SetScale(float scale) { o_scale = scale; }
	/** @brief Returns the sprite diameter (see SetScale for its unit). */
	float GetScale() const { return o_scale; }

	/** @brief Sets the opacity (0.0 = transparent, 1.0 = opaque). */
	void SetOpacity(float opacity) { o_opacity = opacity; }
	/** @brief Returns the opacity value. */
	float GetOpacity() const { return o_opacity; }

	/**
	 * @brief Sets the size projection mode.
	 * @param mode 0 = screen space (constant pixel size), 1 = world space
	 *             (shrinks with distance).
	 */
	void SetProjectionMode(int mode) { o_projectionMode = mode; }
	/** @brief Returns the projection mode. */
	int GetProjectionMode() const { return o_projectionMode; }

	/** @brief Enables x-ray mode: skip the depth test so the points are never occluded. */
	void SetXRay(bool xray) { o_xray = xray; }
	/** @brief Returns whether x-ray mode is enabled. */
	bool GetXRay() const { return o_xray; }

private:
	PointType o_pointType{PointType::SQUARE};    ///< Point sprite shape.
	glm::vec4 o_color{1.0f, 1.0f, 1.0f, 1.0f};  ///< Point RGBA color.
	float o_scale{8.0f};                          ///< Sprite diameter (px or world units).
	float o_opacity{1.0f};                        ///< Opacity (0-1).
	int o_projectionMode{0};                      ///< 0 = screen space, 1 = world space.
	bool o_xray{false};                           ///< Draw on top, ignoring depth.

	std::vector<glm::vec3> o_points;              ///< Point positions.
};

} // namespace neurus

