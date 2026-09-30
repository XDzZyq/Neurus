/**
 * @file Camera.h
 * @brief Camera scene object providing view and projection matrices.
 *
 * Camera is a scene object that defines viewpoint and projection for rendering.
 * It inherits from ObjectID (scene graph identity) and Transform3D (spatial placement).
 *
 * Architecture:
 * - Owned by scene graph containers
 * - Active camera selected via scene manager
 * - Renderer reads camera matrices as immutable data
 * - Editor manipulates camera via CameraController events
 *
 * @note Multiple cameras can exist, but only one is active at a time.
 * @note Perspective projection only. Orthographic not yet supported.
 */

#pragma once

#include "scene/Transform.h"
#include "scene/ObjectID.h"

#include <cereal/types/base_class.hpp>
#include <cereal/types/vector.hpp>

#include <vector>

namespace neurus {

/**
 * @brief Camera object defining viewpoint and projection for rendering.
 *
 * Camera provides perspective projection with configurable FOV, aspect ratio,
 * and near/far clip planes. Transform3D provides position/rotation via standard
 * scene graph interface.
 *
 * View and projection matrices are recomputed eagerly whenever a camera
 * parameter changes (FOV, aspect, near/far, position, target).
 * Camera data can be serialized to float array for GPU upload.
 *
 * @note Inheritance: Inherits identity from ObjectID, transform from Transform3D.
 */
class Camera : public ObjectID, public Transform3D
{
public:
	/** @brief GOType tag for runtime type discrimination. */
	static constexpr ObjectID::GOType Type = ObjectID::GOType::GO_CAM;

	/** @brief Viewport width in pixels. */
	float cam_w = 1.0f;

	/** @brief Viewport height in pixels. */
	float cam_h = 1.0f;

	/** @brief Perspective field of view in degrees. */
	float cam_pers = 60.0f;

	/** @brief Near clipping plane distance. */
	float cam_near = 0.1f;

	/** @brief Far clipping plane distance. */
	float cam_far = 100.0f;

	/** @brief Look-at target position. */
	glm::vec3 cam_tar = glm::vec3(0.0f, 0.0f, 0.0f);

	/** @brief Serialized camera data for GPU upload. */
	std::vector<float> cam_floatData;

	/**
	 * @brief Constructs a camera with explicit parameters.
	 * @param w Viewport width in pixels
	 * @param h Viewport height in pixels
	 * @param per Perspective FOV in degrees
	 * @param n Near clipping plane distance
	 * @param f Far clipping plane distance
	 */
	Camera(float w, float h, float per, float n, float f);

	/**
	 * @brief Constructs a camera with default parameters.
	 * @note Defaults: FOV=60°, near=0.1f, far=100.0f, w=1, h=1.
	 */
	Camera();

	/**
	 * @brief Deleted copy constructor - Camera identity must be unique.
	 */
	Camera(const Camera&) = delete;

	/**
	 * @brief Deleted copy assignment - Camera identity must be unique.
	 */
	Camera& operator=(const Camera&) = delete;

	/**
	 * @brief Default move constructor.
	 */
	Camera(Camera&&) = default;

	/**
	 * @brief Default move assignment.
	 */
	Camera& operator=(Camera&&) = default;

	/**
	 * @brief Destroys the camera.
	 */
	~Camera() override = default;

	/**
	 * @brief Cereal serialization for camera.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note Cached projection matrix and frustum dirty flag are not
	 *       serialized: they are computed values, rebuilt by the `load` branch
	 *       below.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<ObjectID>(this),
		   cereal::make_nvp("transform", cereal::base_class<Transform3D>(this)),
		   CEREAL_NVP(cam_w), CEREAL_NVP(cam_h),
		   CEREAL_NVP(cam_pers), CEREAL_NVP(cam_near), CEREAL_NVP(cam_far),
		   CEREAL_NVP(cam_tar), CEREAL_NVP(cam_floatData));

		// o_cachedView/o_cachedProj are caches of the fields above, and every setter
		// that maintains them (SetPosition/SetTarPos/ChangeCamRatio/ChangeCamPersp)
		// is bypassed by deserialization, so the load has to rebuild them. Nothing
		// else in the load path rebuilds the VIEW matrix: FinishLoad() reaches
		// RecomputeMatrices() through ApplyViewportToActiveCamera() ->
		// ChangeCamRatio(), which is the only reason a stale view ever stayed
		// latent. Same idiom as Transform3D/ImageData/MeshData/UID.
		if constexpr (Archive::is_loading::value)
			RecomputeMatrices();
	}

	/**
	 * @brief Returns the cached view matrix (lookAt from position to target).
	 *
	 * Recomputed eagerly whenever camera position or target changes.
	 * Uses glm::lookAt with the camera position (from Transform3D),
	 * the look-at target, and world up vector (0, 0, 1) (Z-up).
	 *
	 * @return 4x4 view matrix.
	 */
	glm::mat4 GetViewMatrix() const;

	/**
	 * @brief Returns the cached perspective projection matrix.
	 *
	 * Recomputed eagerly whenever FOV, aspect ratio, near, or far changes.
	 * Uses glm::perspective with Y-flip for Vulkan NDC.
	 *
	 * @return 4x4 projection matrix.
	 */
	glm::mat4 GetProjectionMatrix() const;

	/**
	 * @brief Updates the viewport aspect ratio from width and height.
	 * @param w New viewport width in pixels
	 * @param h New viewport height in pixels
	 * @note Projection and view matrices are recomputed immediately.
	 */
	void ChangeCamRatio(float w, float h);

	/**
	 * @brief Updates the perspective field of view.
	 * @param persp New FOV in degrees
	 * @note Projection and view matrices are recomputed immediately.
	 */
	void ChangeCamPersp(float persp);

	/**
	 * @brief Sets camera position in world space.
	 * @param pos New camera position
	 * @note Delegates to Transform3D::SetPosition().
	 */
	void SetPosition(const glm::vec3& pos) override;

	/**
	 * @brief Sets the look-at target position.
	 * @param pos Target position to look at
	 */
	void SetTarPos(const glm::vec3& pos);

	/**
	 * @brief Serializes camera parameters to float vector for GPU upload.
	 *
	 * Populates cam_floatData with 8 floats:
	 * - 0-2: position (x, y, z)
	 * - 3-5: rotation (pitch, yaw, roll in degrees)
	 * - 6: aspect ratio (w / h)
	 * - 7: FOV in radians
	 */
	void GenFloatData();

	/**
	 * @brief Returns pointer to Camera as a Transform for polymorphic access.
	 * @return Non-owning void pointer to the Transform base.
	 * @note Overrides ObjectID::GetTransform().
	 */
	void* GetTransform() override;

private:
	/** @brief Recomputes view and projection matrices from current parameters. */
	void RecomputeMatrices();

	glm::mat4 o_cachedView{1.0f};     ///< Cached view matrix (lookAt).
	glm::mat4 o_cachedProj{1.0f};     ///< Cached projection matrix (perspective).
};

} // namespace neurus

