/**
 * @file Transform.h
 * @brief Transform components for 3D spatial representation.
 *
 * Provides Transform base class and Transform3D implementation for
 * position/rotation/scale (TRS) matrix computation. The model matrix is
 * recomputed eagerly on every SetPosition/SetRotation/SetScale call.
 *
 * @note All scene objects with spatial placement should inherit Transform3D.
 * @note Transforms are not thread-safe. Access from main thread only.
 */

#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/type_ptr.hpp"

#include <cereal/cereal.hpp>

#include "core/Serialize.h"

#include "GlmSerialization.h"

namespace neurus {

/**
 * @brief Abstract base class for transform components.
 *
 * Provides the polymorphic interface for spatial transforms; derived classes
 * implement the dimension-specific matrix math.
 */
class Transform
{
public:
	virtual ~Transform() = default;

	/**
	 * @brief Returns a pointer to this transform for polymorphic access.
	 * @return Non-owning pointer to this Transform instance.
	 */
	virtual Transform* GetTransformPtr() { return this; }

	// Non-copyable (RAII resource ownership semantics)
	Transform(const Transform&) = delete;
	Transform& operator=(const Transform&) = delete;

	// Movable
	Transform(Transform&&) = default;
	Transform& operator=(Transform&&) = default;

protected:
	Transform() = default;
};

/**
 * @brief 3D transform component with TRS (translate/rotate/scale) matrix computation.
 *
 * Holds position, rotation (Euler degrees, pitch=X, yaw=Z, roll=Y) and scale.
 * Coordinate system: right-handed, Z-up; X = right, Y = forward, Z = up;
 * local forward is +Y. Model = T * Rz(yaw) * Rx(pitch) * Ry(roll) * S.
 *
 * The matrix is recomputed eagerly whenever a component changes - including
 * once after a load - so it is always up to date on access.
 */
class Transform3D : public Transform
{
public:
	/**
	 * @brief Constructs a Transform3D with identity values.
	 */
	Transform3D() = default;

	/**
	 * @brief Destroys the Transform3D.
	 */
	~Transform3D() override = default;

	/**
	 * @brief Cereal serialization for TRS transform.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note Only position/rotation/scale are serialized; `o_modelMatrix` is a
	 *       computed value, so a load recomputes it.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		archive::Optional(ar, cereal::make_nvp("m_position", o_position),
		                  cereal::make_nvp("m_rotation", o_rotation),
		                  cereal::make_nvp("m_scale", o_scale));

		// `o_modelMatrix` is not persisted and the setters that normally
		// recompute it are bypassed by deserialization, so a load would
		// otherwise keep the identity matrix the transform was constructed
		// with.
		if constexpr (Archive::is_loading::value)
			RecomputeModelMatrix();
	}

	/**
	 * @brief Returns a typed pointer to this Transform3D.
	 * @return Non-owning pointer to this Transform3D instance.
	 */
	virtual Transform3D* GetTransformPtr() override { return this; }

	// -----------------------------------------------------------------------
	// Setters - eagerly recompute the model matrix
	// -----------------------------------------------------------------------

	/**
	 * @brief Sets the world position.
	 * @param pos New position in world space.
	 */
	virtual void SetPosition(const glm::vec3& pos);

	/**
	 * @brief Sets the rotation as Euler angles.
	 * @param degrees Rotation in degrees (pitch=X, yaw=Z, roll=Y).
	 */
	virtual void SetRotation(const glm::vec3& degrees);

	/**
	 * @brief Sets the local scale.
	 * @param scale Per-axis scale factors.
	 */
	virtual void SetScale(const glm::vec3& scale);

	// -----------------------------------------------------------------------
	// Getters
	// -----------------------------------------------------------------------

	/**
	 * @brief Returns the current position.
	 * @return Const reference to the position vector.
	 */
	const glm::vec3& GetPosition() const { return o_position; }

	/**
	 * @brief Returns the current rotation in degrees.
	 * @return Const reference to the rotation vector (pitch=X, yaw=Z, roll=Y).
	 */
	const glm::vec3& GetRotation() const { return o_rotation; }

	/**
	 * @brief Returns the current scale.
	 * @return Const reference to the scale vector.
	 */
	const glm::vec3& GetScale() const { return o_scale; }

	// -----------------------------------------------------------------------
	// Matrix computation
	// -----------------------------------------------------------------------

	/**
	 * @brief Returns the model matrix (TRS).
	 *
	 * Composition: Translate * Rz(yaw) * Rx(pitch) * Ry(roll) * Scale.
	 *
	 * @return 4x4 model matrix.
	 */
	glm::mat4 GetModelMatrix() const;

	/**
	 * @brief Computes the forward direction vector.
	 *
	 * Rotates local forward +Y by the current Euler rotation
	 * (yaw=Z, pitch=X, roll=Y); returns a normalized vector.
	 *
	 * @return Normalized forward direction vector.
	 */
	glm::vec3 GetDirection() const;

	/**
	 * @brief Computes the normal matrix for transforming surface normals.
	 *
	 * Inverse-transpose of the model matrix's upper-left 3x3, so normals stay
	 * correct under non-uniform scaling.
	 *
	 * @return 3x3 normal matrix.
	 */
	glm::mat3 GetNormalMatrix() const;

private:
	/** @brief Recomputes o_modelMatrix from the current TRS values. */
	void RecomputeModelMatrix();

	glm::vec3 o_position{0.0f};    ///< World position.
	glm::vec3 o_rotation{0.0f};    ///< Euler rotation in degrees (pitch=X, yaw=Z, roll=Y).
	glm::vec3 o_scale{1.0f};       ///< Per-axis scale.

	glm::mat4 o_modelMatrix{1.0f}; ///< Model matrix: rebuilt by every setter and by serialize().
};

} // namespa

