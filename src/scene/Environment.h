/**
 * @file Environment.h
 * @brief IBL (Image-Based Lighting) environment map object for the scene.
 *
 * A dome light / skybox providing image-based lighting via diffuse irradiance
 * and specular prefiltered cubemaps. CPU-side only: it wraps a shared
 * ImageData for the source equirectangular HDR and holds serializable
 * parameters (intensity, rotation).
 *
 * @note Environment holds no file paths: the source path is an ImageData
 *       concern, so only pooled ImageData instances are wrapped here.
 * @note Serialization stores the pooled ImageData UID, not GPU resources.
 */

#pragma once

#include <memory>
#include <string>

#include "scene/ObjectID.h"
#include "Transform.h"

namespace neurus
{
	
class ImageData;

/**
 * @brief IBL environment map providing image-based lighting for the scene.
 *
 * Stores CPU-side pixel data for an HDR equirectangular map plus serializable
 * intensity and rotation; Transform3D supplies the skybox orientation.
 *
 * @note Not thread-safe. Access from main thread only.
 */
class Environment : public ObjectID, public Transform3D
{
public:
	/** @brief GOType tag for runtime type discrimination. */
	static constexpr ObjectID::GOType Type = ObjectID::GOType::GO_ENVIR;

	/**
	 * @brief Constructs an Environment with default IBL parameters.
	 */
	Environment();

	/**
	 * @brief Constructs an environment wrapping pooled equirect data.
	 * @param data Shared ImageData (pooled resource) for the equirect map.
	 * @note Records the pooled UID for persistence; no path is accepted.
	 */
	explicit Environment(std::shared_ptr<ImageData> data);

	/**
	 * @brief Virtual destructor for polymorphic cleanup.
	 */
	~Environment() override;

	// Non-copyable (like all scene objects)
	Environment(const Environment&) = delete;
	Environment& operator=(const Environment&) = delete;

	// -----------------------------------------------------------------------
	// Equirectangular data (CPU-side)
	// -----------------------------------------------------------------------

	/**
	 * @brief Returns the CPU-side equirectangular pixel data (shared).
	 */
	std::shared_ptr<ImageData> GetEquirectData() const { return o_equirectData; }

	/**
	 * @brief Sets the CPU-side equirectangular pixel data.
	 * @param data Shared ImageData to reference (pooled resource).
	 * @note Records the pooled ID for persistence.
	 * @note Out-of-line: needs the complete ImageData type (GetObjectID).
	 */
	void SetEquirectData(std::shared_ptr<ImageData> data);

	// -----------------------------------------------------------------------
	// Intensity
	// -----------------------------------------------------------------------

	/**
	 * @brief Sets the IBL intensity multiplier.
	 * @param i Intensity scale factor (1.0 = physical).
	 */
	void SetIntensity(float i) { o_intensity = i; }

	/**
	 * @brief Returns the current IBL intensity multiplier.
	 * @return Intensity scale factor.
	 */
	float GetIntensity() const { return o_intensity; }

	// -----------------------------------------------------------------------
	// Rotation
	// -----------------------------------------------------------------------

	/**
	 * @brief Sets the environment map rotation (Y-axis).
	 * @param r Rotation angle in degrees around the Y axis.
	 */
	void SetRotation(float r) { o_rotation = r; }

	/**
	 * @brief Returns the current environment map rotation.
	 * @return Rotation angle in degrees.
	 */
	float GetRotation() const { return o_rotation; }

	// -----------------------------------------------------------------------
	// Virtual overrides
	// -----------------------------------------------------------------------

	/**
	 * @brief Returns this object's Transform component.
	 * @return Void pointer to the inherited Transform3D.
	 */
	void* GetTransform() override
	{
		return static_cast<Transform3D*>(this);
	}

	/**
	 * @brief Environment does not own a shader.
	 * @return nullptr always.
	 */
	void* GetShader() override
	{
		return nullptr;
	}

	/**
	 * @brief Environment does not own a material.
	 * @return nullptr always.
	 */
	void* GetMaterial() override
	{
		return nullptr;
	}

	/// Pooled ImageData UID (0 = none); the serialized form of o_equirectData.
	int o_imageDataId = 0;

	// -----------------------------------------------------------------------
	// Serialization (Cereal)
	// -----------------------------------------------------------------------

	/**
	 * @brief Cereal serialization for Environment.
	 *
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note Stores the pooled ImageData UID, not texture data.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<ObjectID>(this),
		   cereal::make_nvp("transform", cereal::base_class<Transform3D>(this)));
		archive::Optional(ar, CEREAL_NVP(o_imageDataId),
		                  cereal::make_nvp("m_intensity", o_intensity),
		                  cereal::make_nvp("m_rotation", o_rotation));
	}

private:
	float       o_intensity = 1.0f;   ///< IBL intensity multiplier
	float       o_rotation  = 0.0f;   ///< Y-axis rotation in degrees

	std::shared_ptr<ImageData> o_equirectData;  ///< CPU-side equirectangular pixel data
};

} // namespace neurus

