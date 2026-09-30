/**
 * @file Transform.h
 * @brief Transform components for 3D spatial representation.
 *
 * Provides Transform base class and Transform3D implementation for
 * position/rotation/scale (TRS) matrix computation. The model matrix is
 * recomputed eagerly on every SetPosition/SetRotation/SetScale call.
 *
 * Architecture:
 * - Transform is abstract base with virtual GetTransformPtr() for polymorphic access
 * - Transform3D provides Euler-angle 3D transforms using glm::mat4
 * - Model matrix is always up-to-date (eager computation)
 *
 * Transform Composition:
 * - Model matrix = Translate * RotateX * RotateY * RotateZ * Scale
 * - Normal matrix = inverse-transpose of upper-left 3x3 model matrix
 *
 * @note All scene objects with spatial placement should inherit Transform3D.
 * @note Transforms are not thread-safe. Access from main thread only.
 */

#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/type_ptr.hpp"

#include <cereal/cereal.hpp>

#include "GlmSerialization.h"

namespace neurus {

/**
 * @brief Abstract base class for transform components.
 *
 * Provides the polymorphic interface for spatial transforms.
 * Derived classes implement dimension-specific matrix math.
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
 * Transform3D manages position, rotation (Euler angles in degrees), and scale
 * for 3D scene objects. The model matrix is computed eagerly whenever a
 * component changes - including once after deserialization - so it is always
 * up-to-date on access.
 *
 * Coordinate System:
 * - Right-handed coordinate system, Z-up
 * - X = right, Y = forward, Z = up
 * - Euler rotation order: pitch (X), yaw (Z), roll (Y)
 * - Rotation angles stored in degrees, converted to radians at compute time
 *
 * Model Matrix Composition:
 * - Model = T(position) * Rz(yaw) * Rx(pitch) * Ry(roll) * S(scale)
 * - Applied to vertex v: T * (Rz * (Rx * (Ry * (S * v))))
 * - Order: scale, roll (Y), pitch (X), yaw (Z), translate
 *
 * Forward Direction:
 * - Local forward = +Y (0, 1, 0). GetDirection() rotates to world-space.
 * - Up = +Z (0, 0, 1). Yaw rotates around Z.
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
		ar(cereal::make_nvp("m_position", o_position),
		   cereal::make_nvp("m_rotation", o_rotation),
		   cereal::make_nvp("m_scale", o_scale));

		// `o_modelMatrix` is not persisted, and the setters - the only place it is
		// ever recomputed - are bypassed by deserialization. Without this a loaded
		// transform keeps the identity matrix it was constructed with, so the
		// Property panel reads the loaded TRS while GeometryPass, ShadowDepthPass
		// and DebugDrawBuilder draw the object at the origin with unit scale.
		// Same idiom as ImageData/MeshData/UID/RenderShader.
		if constexpr (Archive::is_loading::value)
			RecomputeModelMatrix();
	}

	/**
	 * @brief Returns a typed pointer to this Transform3D.
	 * @return Non-owning pointer to this Transform3D instance.
	 */
	virtual Transform3D* GetTransformPtr() override { return this; }

	// -----------------------------------------------------------------------
	// Setters - mark cached matrix dirty
	// -----------------------------------------------------------------------

	/**
	 * @brief Sets the world position.
	 * @param pos New position in world space.
	 * @note Model matrix is recomputed immediately.
	 */
	virtual void SetPosition(const glm::vec3& pos);

	/**
	 * @brief Sets the rotation as Euler angles.
	 * @param degrees Rotation in degrees (pitch=X, yaw=Z, roll=Y).
	 * @note Model matrix is recomputed immediately.
	 */
	virtual void SetRotation(const glm::vec3& degrees);

	/**
	 * @brief Sets the local scale.
	 * @param scale Per-axis scale factors.
	 * @note Model matrix is recomputed immediately.
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
	 * The matrix is always up-to-date — recomputed eagerly whenever a component
	 * changes, and once after a load. Composition: Translate * Rotate * Scale
	 * with rotation applied in ZXY order (yaw=Z, pitch=X, roll=Y).
	 *
	 * @return 4x4 model matrix.
	 */
	glm::mat4 GetModelMatrix() const;

	/**
	 * @brief Computes the local-space forward direction vector.
	 *
	 * Rotates the forward vector (0,1,0) by the current Euler rotation
	 * (yaw=Z, pitch=X, roll=Y). Returns a normalized direction vector.
	 * When no rotation is applied, returns (0,1,0) (+Y forward).
	 *
	 * @return Normalized forward direction vector.
	 */
	glm::vec3 GetDirection() const;

	/**
	 * @brief Computes the normal matrix for transforming surface normals.
	 *
	 * The normal matrix is the inverse transpose of the upper-left 3x3
	 * portion of the model matrix. This ensures correct normal orientation
	 * under non-uniform scaling.
	 *
	 * @return 3x3 normal matrix.
	 */
	glm::mat3 GetNormalMatrix() const;

private:
	/**
	 * @brief Recomputes o_modelMatrix from the current TRS.
	 *
	 * The only two writers of the TRS members are the setters and the `load`
	 * branch of serialize(); both go through here, so the cached matrix cannot
	 * go stale behind their back.
	 */
	void RecomputeModelMatrix();

	glm::vec3 o_position{0.0f};    ///< World position.
	glm::vec3 o_rotation{0.0f};    ///< Euler rotation in degrees (pitch=X, yaw=Z, roll=Y).
	glm::vec3 o_scale{1.0f};       ///< Per-axis scale.

	glm::mat4 o_modelMatrix{1.0f}; ///< Model matrix: rebuilt by every setter and by serialize().
};

} // namespa

