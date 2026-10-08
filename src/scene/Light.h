/**
 * @file Light.h
 * @brief Light source objects for PBR lighting.
 *
 * Provides the Light class (point, sun, spot, area lights).
 * All GPU buffer management, shadow map rendering, and shadow computation
 * are handled by ShadowSystem (future: src/render/ShadowSystem.h).
 *
 * Architecture:
 * - Light inherits ObjectID (scene identity) and Transform3D (spatial placement)
 * - ShadowSystem converts scene lights into GPU-friendly buffers
 * - ShadowSystem owns all shadow map rendering resources and projection matrices
 */

#pragma once

#include <string>
#include <utility>

#include "glm/glm.hpp"

#include <cereal/types/base_class.hpp>

#include "core/Serialize.h"
#include "scene/Transform.h"
#include "scene/ObjectID.h"

namespace neurus
{

// Forward declaration for sprite icon type (used by editor viewport gizmos).
// Full definition is in scene/Sprite.h.
enum SpriteType : int;

/**
 * @brief Enumeration of supported light types.
 */
enum LightType : int
{
	NONELIGHT = -1,  ///< Invalid or uninitialized light
	POINTLIGHT,      ///< Omnidirectional point light
	SUNLIGHT,        ///< Directional sun light (parallel rays)
	SPOTLIGHT,       ///< Cone-shaped spot light
	AREALIGHT        ///< Rectangular area light with soft shadows
};

/**
 * @brief Light source object supporting point, sun, spot, and area lights.
 *
 * Light provides PBR lighting with configurable type, color, power, and
 * shadow flags. Transform3D determines position/orientation, while
 * type-specific parameters control light shape (radius for point, cutoff
 * for spot, ratio for area).
 *
 * @note Shadow map rendering, projection matrices, and GPU resources are
 *       managed by ShadowSystem (future: src/render/ShadowSystem.h).
 * @note Inheritance: ObjectID for scene identity, Transform3D for spatial
 *       placement.
 * @note Thread-safety: Not thread-safe. Access from main thread only.
 */
class Light : public ObjectID, public Transform3D
{
public:
	/** @brief GOType tag for runtime type discrimination. */
	static constexpr ObjectID::GOType Type = ObjectID::GOType::GO_LIGHT;

	// -----------------------------------------------------------------------
	// Public members (light state)
	// -----------------------------------------------------------------------

	/** @brief Enable shadow casting for this light. */
	bool use_shadow{ true };

	/** @brief Type of light source. */
	LightType light_type{ LightType::NONELIGHT };

	/** @brief Luminous intensity (arbitrary units). */
	float light_power{ 10.0f };

	/** @brief RGB color multiplier (linear space). */
	glm::vec3 light_color{ 1.0f };

	// Point light parameters
	/** @brief Physical radius for soft shadows (point lights only). */
	float light_radius{ 0.01f };

	// Spot light parameters
	/** @brief Inner cone cosine (full brightness). */
	float spot_cutoff{ 0.9f };

	/** @brief Outer cone cosine (falloff to zero). */
	float spot_outer_cutoff{ 0.8f };

	// Area light parameters
	/** @brief Aspect ratio (width/height) of rectangular area light. */
	float area_ratio{ 1.0f };

	// -----------------------------------------------------------------------
	// Static shadow parameters (constexpr - compile-time constants)
	// -----------------------------------------------------------------------

	/** @brief Orthographic projection field size for sun shadows. */
	static constexpr float sun_shadow_field = 2.5f;

	/** @brief Near plane for sun shadow map (view-space, relative to eye). */
	static constexpr float sun_shadow_near = -10.0f;

	/** @brief Far plane for sun shadow map (view-space, relative to eye).
	 *  Extended past the geometry so depth is not crammed at NDC Z=1.0. */
	static constexpr float sun_shadow_far = 30.0f;

	/** @brief Eye distance from the shadow ortho center (decoupled from far
	 *  plane so geometry sits at mid-depth instead of the far boundary). */
	static constexpr float sun_depth_range = 10.0f;

	/** @brief Near plane for point light shadow map. */
	static constexpr float point_shadow_near = 0.1f;

	/** @brief Far plane for point light shadow map. */
	static constexpr float point_shadow_far = 10.0f; ///< @note Hardcoded to 10.0 per ShadowDepthPass contract.

	/** @brief Blur kernel size for soft point shadows. */
	static constexpr float point_blur_range = 0.02f;

	/** @brief Near plane for spot light shadow map. */
	static constexpr float spot_shadow_near = 0.1f;

	/** @brief Far plane for spot light shadow map. */
	static constexpr float spot_shadow_far = 100.0f;

	/** @brief Blur kernel size for soft spot shadows. */
	static constexpr float spot_blur_range = 0.02f;

	/** @brief Near plane for area light shadow map. */
	static constexpr float area_shadow_near = 0.1f;

	/** @brief Far plane for area light shadow map. */
	static constexpr float area_shadow_far = 100.0f;

	/** @brief Blur kernel size for soft area shadows. */
	static constexpr float area_blur_range = 0.04f;

public:
	/**
	 * @brief Cereal serialization for light.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note Static constexpr shadow parameters are compile-time constants
	 *       and are not serialized.
	 * @note `light_type` stays strict while every other value field is optional:
	 *       it discriminates point from sun, and `Light()` leaves it at
	 *       `NONELIGHT`, so defaulting it would load a dead light rather than a
	 *       slightly wrong one.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<ObjectID>(this),
		   cereal::make_nvp("transform", cereal::base_class<Transform3D>(this)));
		// Split around light_type only to keep the written key order unchanged;
		// the reader is name-keyed, so the split costs nothing on load.
		NEURUS_OPTIONAL(ar, use_shadow);
		ar(CEREAL_NVP(light_type));
		archive::Optional(ar, CEREAL_NVP(light_power), CEREAL_NVP(light_color),
		                  CEREAL_NVP(light_radius), CEREAL_NVP(spot_cutoff),
		                  CEREAL_NVP(spot_outer_cutoff), CEREAL_NVP(area_ratio));
	}

public:
	/**
	 * @brief Constructs a light with default values (type NONELIGHT).
	 */
	Light();

	/**
	 * @brief Constructs a light with specified type, power, and color.
	 * @param type Light type (POINTLIGHT, SUNLIGHT, SPOTLIGHT, AREALIGHT).
	 * @param power Luminous intensity (default: 10).
	 * @param color RGB color in linear space (default: white).
	 */
	Light(LightType type, float power = 10.0f, glm::vec3 color = glm::vec3(1.0f));

	/**
	 * @brief Parses light type to sprite icon and display name.
	 * @param _type Light type to parse.
	 * @return Pair of (sprite icon type, display name string).
	 * @note Used by editor to display light icons in viewport and outliner.
	 */
	static std::pair<SpriteType, std::string> ParseLightName(LightType _type);

	// -----------------------------------------------------------------------
	// Setters - mark is_light_changed dirty flag
	// -----------------------------------------------------------------------

	/** @brief Dirty flag indicating light parameters changed since last frame. */
	bool is_light_changed{ false };

	/**
	 * @brief Sets light color.
	 * @param _col New RGB color in linear space.
	 */
	void SetColor(const glm::vec3& _col);

	/**
	 * @brief Sets light power/intensity.
	 * @param _power New luminous intensity.
	 */
	void SetPower(float _power);

	/**
	 * @brief Enables or disables shadow casting.
	 * @param _state True to enable shadows, false to disable.
	 */
	void SetShadow(bool _state);

	/**
	 * @brief Sets point light radius.
	 * @param _rad New radius for soft shadow computation.
	 * @note Only applicable to POINTLIGHT type.
	 */
	void SetRadius(float _rad);

	/**
	 * @brief Sets spot light inner cutoff angle (cosine).
	 * @param _ang Cosine of inner cone angle.
	 * @note Only applicable to SPOTLIGHT type.
	 */
	void SetCutoff(float _ang);

	/**
	 * @brief Sets spot light outer cutoff angle (cosine).
	 * @param _ang Cosine of outer cone angle (defines falloff region).
	 * @note Only applicable to SPOTLIGHT type.
	 */
	void SetOuterCutoff(float _ang);

	/**
	 * @brief Sets area light aspect ratio.
	 * @param _ratio Width/height ratio of rectangular area light.
	 * @note Only applicable to AREALIGHT type.
	 */
	void SetRatio(float _ratio);

public:
	/**
	 * @brief Returns typed pointer to this object's Transform component.
	 * @return Void pointer to Transform (downcast from Transform3D).
	 * @note Overrides ObjectID::GetTransform() for polymorphic transform access.
	 */
	void* GetTransform() override
	{
		return GetTransformPtr();
	}
};

} // namespace neurus

