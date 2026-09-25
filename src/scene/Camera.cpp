/**
 * @file Camera.cpp
 * @brief Implementation of Camera scene object.
 */

#include "scene/Camera.h"

namespace neurus {

// -----------------------------------------------------------------------
// Constructors
// -----------------------------------------------------------------------

Camera::Camera(float w, float h, float per, float n, float f)
	: cam_w(w)
	, cam_h(h)
	, cam_pers(per)
	, cam_near(n)
	, cam_far(f)
{
	o_type = ObjectID::GOType::GO_CAM;
	o_name = "Camera";
	RecomputeMatrices();
}

Camera::Camera()
{
	o_type = ObjectID::GOType::GO_CAM;
	o_name = "Camera";
	RecomputeMatrices();
}

// -----------------------------------------------------------------------
// Internal: recompute view + projection matrices eagerly
// -----------------------------------------------------------------------

void Camera::RecomputeMatrices()
{
	o_cachedView = glm::lookAt(GetPosition(), cam_tar, glm::vec3(0.0f, 0.0f, 1.0f)); // Z-up

	const float aspect = cam_w / cam_h;
	glm::mat4 projection = glm::perspective(
			glm::radians(cam_pers), aspect, cam_near, cam_far);
	projection[1][1] *= -1.0f;   // Flip Y for Vulkan NDC (Y=-1 at top)
	o_cachedProj = projection;
}

// -----------------------------------------------------------------------
// View / Projection matrix access
// -----------------------------------------------------------------------

glm::mat4 Camera::GetViewMatrix() const
{
	return o_cachedView;
}

glm::mat4 Camera::GetProjectionMatrix() const
{
	return o_cachedProj;
}

// -----------------------------------------------------------------------
// Parameter setters — eagerly recompute matrices
// -----------------------------------------------------------------------

void Camera::ChangeCamRatio(float w, float h)
{
	cam_w = w;
	cam_h = h;
	RecomputeMatrices();
}

void Camera::ChangeCamPersp(float persp)
{
	cam_pers = persp;
	RecomputeMatrices();
}

void Camera::SetPosition(const glm::vec3& pos)
{
	Transform3D::SetPosition(pos);
	// View matrix depends on position, so recompute eagerly
	// (SetPosition already recomputed the model matrix)
	RecomputeMatrices();
}

void Camera::SetTarPos(const glm::vec3& pos)
{
	cam_tar = pos;
	RecomputeMatrices();
}

// -----------------------------------------------------------------------
// Serialization
// -----------------------------------------------------------------------

void Camera::GenFloatData()
{
	cam_floatData.resize(8);

	const glm::vec3& pos = GetPosition();
	const glm::vec3& rot = GetRotation();

	cam_floatData[0] = pos.x;
	cam_floatData[1] = pos.y;
	cam_floatData[2] = pos.z;

	cam_floatData[3] = rot.x;
	cam_floatData[4] = rot.y;
	cam_floatData[5] = rot.z;

	cam_floatData[6] = cam_w / cam_h;
	cam_floatData[7] = glm::radians(cam_pers);
}

// -----------------------------------------------------------------------
// Transform access
// -----------------------------------------------------------------------

void* Camera::GetTransform()
{
	return static_cast<Transform*>(this);
}

} // namespace neurus
