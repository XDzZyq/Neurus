/**
 * @file Transform.cpp
 * @brief Implementation of Transform3D methods.
 *
 * Provides TRS matrix computation, direction vector calculation,
 * and normal matrix derivation from Euler angle rotation.
 *
 * Coordinate System: Z-up, right-hand, +Y forward.
 *   - X = right, Y = forward, Z = up
 *   - Euler order: pitch (X), yaw (Z), roll (Y)
 *   - Model matrix: T * Rz(yaw) * Rx(pitch) * Ry(roll) * S
 */

#include "scene/Transform.h"

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

namespace neurus {

// -----------------------------------------------------------------------
// Internal: compute model matrix from current TRS components
// -----------------------------------------------------------------------

static glm::mat4 ComputeModelMatrix(const glm::vec3& position,
                                    const glm::vec3& rotation,
                                    const glm::vec3& scale)
{
	glm::mat4 mat{1.0f};
	const glm::vec3 rad = glm::radians(rotation);

	// TRS: T * Rz(yaw) * Rx(pitch) * Ry(roll) * S
	mat = glm::translate(mat, position);
	mat = glm::rotate(mat, rad.z, glm::vec3(0.0f, 0.0f, 1.0f)); // Yaw   (Z = up)
	mat = glm::rotate(mat, rad.x, glm::vec3(1.0f, 0.0f, 0.0f)); // Pitch (X)
	mat = glm::rotate(mat, rad.y, glm::vec3(0.0f, 1.0f, 0.0f)); // Roll  (Y = forward)
	mat = glm::scale(mat, scale);

	return mat;
}

// -----------------------------------------------------------------------
// Setters — eagerly recompute model matrix
// -----------------------------------------------------------------------

void Transform3D::RecomputeModelMatrix()
{
	o_modelMatrix = ComputeModelMatrix(o_position, o_rotation, o_scale);
}

void Transform3D::SetPosition(const glm::vec3& pos)
{
	o_position = pos;
	RecomputeModelMatrix();
}

void Transform3D::SetRotation(const glm::vec3& degrees)
{
	o_rotation = degrees;
	RecomputeModelMatrix();
}

void Transform3D::SetScale(const glm::vec3& scale)
{
	o_scale = scale;
	RecomputeModelMatrix();
}

// -----------------------------------------------------------------------
// Matrix access
// -----------------------------------------------------------------------

glm::mat4 Transform3D::GetModelMatrix() const
{
	return o_modelMatrix;
}

glm::vec3 Transform3D::GetDirection() const
{
	// Build rotation matrix matching GetModelMatrix() order: Rz * Rx * Ry
	const glm::vec3 rad = glm::radians(o_rotation);
	glm::mat4 rot{1.0f};
	rot = glm::rotate(rot, rad.z, glm::vec3(0.0f, 0.0f, 1.0f)); // Yaw   (Z = up)
	rot = glm::rotate(rot, rad.x, glm::vec3(1.0f, 0.0f, 0.0f)); // Pitch (X)
	rot = glm::rotate(rot, rad.y, glm::vec3(0.0f, 1.0f, 0.0f)); // Roll  (Y = forward)

	// Local forward = +Y (0, 1, 0), rotate by the 3x3 rotation part
	// Ry(roll) has no effect on (0,1,0) since it's axis-aligned.
	// Effective: d = Rz(yaw) * Rx(pitch) * (0,1,0)
	//   d.x = -cos(pitch) * sin(yaw)
	//   d.y =  cos(pitch) * cos(yaw)
	//   d.z =  sin(pitch)
	const glm::vec3 forward = glm::vec3(rot * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
	return glm::normalize(forward);
}

glm::mat3 Transform3D::GetNormalMatrix() const
{
	return glm::transpose(glm::inverse(glm::mat3(o_modelMatrix)));
}

} // namespace neurus
