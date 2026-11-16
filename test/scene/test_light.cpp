/**
 * @file test_light.cpp
 * @brief Unit tests for Light class (point, sun, spot, area).
 *
 * TDD: RED (test written first) → GREEN (implementation verified).
 * All tests are pure CPU math - no GPU required.
 */

#include <gtest/gtest.h>

#include "scene/Light.hpp"

using namespace neurus;

// -----------------------------------------------------------------------
// Default constructor
// -----------------------------------------------------------------------

/**
 * @brief Default-constructed Light has NONELIGHT type, power=10, color=white.
 */
TEST(Light, DefaultConstructor)
{
	Light light;

	EXPECT_EQ(light.light_type, LightType::NONELIGHT);
	EXPECT_FLOAT_EQ(light.light_power, 10.0f);
	EXPECT_EQ(light.light_color, glm::vec3(1.0f));
	EXPECT_TRUE(light.use_shadow);
	EXPECT_FALSE(light.is_light_changed);

	// ObjectID type must be GO_LIGHT
	EXPECT_EQ(light.o_type, ObjectID::GOType::GO_LIGHT);
}

// -----------------------------------------------------------------------
// Typed constructors - all 4 types
// -----------------------------------------------------------------------

/**
 * @brief Construct Light with POINTLIGHT type (default power/color).
 */
TEST(Light, TypedConstructor_Point)
{
	Light light(LightType::POINTLIGHT);

	EXPECT_EQ(light.light_type, LightType::POINTLIGHT);
	EXPECT_FLOAT_EQ(light.light_power, 10.0f);
	EXPECT_EQ(light.light_color, glm::vec3(1.0f));
	EXPECT_EQ(light.o_type, ObjectID::GOType::GO_LIGHT);
}

/**
 * @brief Construct Light with SUNLIGHT type (default power/color).
 */
TEST(Light, TypedConstructor_Sun)
{
	Light light(LightType::SUNLIGHT);

	EXPECT_EQ(light.light_type, LightType::SUNLIGHT);
	EXPECT_FLOAT_EQ(light.light_power, 10.0f);
	EXPECT_EQ(light.light_color, glm::vec3(1.0f));
	EXPECT_EQ(light.o_type, ObjectID::GOType::GO_LIGHT);
}

/**
 * @brief Construct Light with SPOTLIGHT type (default power/color).
 */
TEST(Light, TypedConstructor_Spot)
{
	Light light(LightType::SPOTLIGHT);

	EXPECT_EQ(light.light_type, LightType::SPOTLIGHT);
	EXPECT_FLOAT_EQ(light.light_power, 10.0f);
	EXPECT_EQ(light.light_color, glm::vec3(1.0f));
	EXPECT_EQ(light.o_type, ObjectID::GOType::GO_LIGHT);
}

/**
 * @brief Construct Light with AREALIGHT type (default power/color).
 */
TEST(Light, TypedConstructor_Area)
{
	Light light(LightType::AREALIGHT);

	EXPECT_EQ(light.light_type, LightType::AREALIGHT);
	EXPECT_FLOAT_EQ(light.light_power, 10.0f);
	EXPECT_EQ(light.light_color, glm::vec3(1.0f));
	EXPECT_EQ(light.o_type, ObjectID::GOType::GO_LIGHT);
}

/**
 * @brief Construct Light with custom power and color values.
 */
TEST(Light, TypedConstructor_CustomPowerAndColor)
{
	glm::vec3 blue(0.0f, 0.0f, 1.0f);
	Light light(LightType::POINTLIGHT, 50.0f, blue);

	EXPECT_EQ(light.light_type, LightType::POINTLIGHT);
	EXPECT_FLOAT_EQ(light.light_power, 50.0f);
	EXPECT_EQ(light.light_color, blue);
}

// -----------------------------------------------------------------------
// Type-specific members
// -----------------------------------------------------------------------

/**
 * @brief Point light radius has correct default.
 */
TEST(Light, PointRadiusDefault)
{
	Light light(LightType::POINTLIGHT);
	EXPECT_FLOAT_EQ(light.light_radius, 0.01f);
}

/**
 * @brief Spot light cutoffs have correct defaults.
 */
TEST(Light, SpotCutoffDefaults)
{
	Light light(LightType::SPOTLIGHT);
	EXPECT_FLOAT_EQ(light.spot_cutoff, 0.9f);
	EXPECT_FLOAT_EQ(light.spot_outer_cutoff, 0.8f);
}

/**
 * @brief Area light ratio has correct default.
 */
TEST(Light, AreaRatioDefault)
{
	Light light(LightType::AREALIGHT);
	EXPECT_FLOAT_EQ(light.area_ratio, 1.0f);
}

// -----------------------------------------------------------------------
// Setters
// -----------------------------------------------------------------------

/**
 * @brief SetColor updates light_color and marks dirty.
 */
TEST(Light, SetColor)
{
	Light light(LightType::POINTLIGHT);
	light.is_light_changed = false;

	glm::vec3 red(1.0f, 0.0f, 0.0f);
	light.SetColor(red);

	EXPECT_EQ(light.light_color, red);
	EXPECT_TRUE(light.is_light_changed);
}

/**
 * @brief SetPower updates light_power and marks dirty.
 */
TEST(Light, SetPower)
{
	Light light(LightType::POINTLIGHT);
	light.is_light_changed = false;

	light.SetPower(25.0f);

	EXPECT_FLOAT_EQ(light.light_power, 25.0f);
	EXPECT_TRUE(light.is_light_changed);
}

/**
 * @brief SetShadow disables shadow and marks dirty.
 */
TEST(Light, SetShadow_Disable)
{
	Light light(LightType::POINTLIGHT);
	EXPECT_TRUE(light.use_shadow);
	light.is_light_changed = false;

	light.SetShadow(false);

	EXPECT_FALSE(light.use_shadow);
	EXPECT_TRUE(light.is_light_changed);
}

/**
 * @brief SetShadow enables shadow and marks dirty.
 */
TEST(Light, SetShadow_Enable)
{
	Light light(LightType::POINTLIGHT);
	light.SetShadow(false);
	EXPECT_FALSE(light.use_shadow);
	light.is_light_changed = false;

	light.SetShadow(true);

	EXPECT_TRUE(light.use_shadow);
	EXPECT_TRUE(light.is_light_changed);
}

/**
 * @brief SetRadius updates light_radius and marks dirty.
 */
TEST(Light, SetRadius)
{
	Light light(LightType::POINTLIGHT);
	light.is_light_changed = false;

	light.SetRadius(0.1f);

	EXPECT_FLOAT_EQ(light.light_radius, 0.1f);
	EXPECT_TRUE(light.is_light_changed);
}

/**
 * @brief SetCutoff updates spot_cutoff and marks dirty.
 */
TEST(Light, SetCutoff)
{
	Light light(LightType::SPOTLIGHT);
	light.is_light_changed = false;

	light.SetCutoff(0.85f);

	EXPECT_FLOAT_EQ(light.spot_cutoff, 0.85f);
	EXPECT_TRUE(light.is_light_changed);
}

/**
 * @brief SetOuterCutoff updates spot_outer_cutoff and marks dirty.
 */
TEST(Light, SetOuterCutoff)
{
	Light light(LightType::SPOTLIGHT);
	light.is_light_changed = false;

	light.SetOuterCutoff(0.75f);

	EXPECT_FLOAT_EQ(light.spot_outer_cutoff, 0.75f);
	EXPECT_TRUE(light.is_light_changed);
}

/**
 * @brief SetRatio updates area_ratio and marks dirty.
 */
TEST(Light, SetRatio)
{
	Light light(LightType::AREALIGHT);
	light.is_light_changed = false;

	light.SetRatio(2.0f);

	EXPECT_FLOAT_EQ(light.area_ratio, 2.0f);
	EXPECT_TRUE(light.is_light_changed);
}

// -----------------------------------------------------------------------
// Dirty flag - initial state (setter dirty checks are in each setter test)
// -----------------------------------------------------------------------

/**
 * @brief Dirty flag starts false after construction.
 */
TEST(Light, DirtyFlag_StartsFalse)
{
	Light light(LightType::POINTLIGHT);
	EXPECT_FALSE(light.is_light_changed);
}

// -----------------------------------------------------------------------
// ParseLightName
// -----------------------------------------------------------------------

/**
 * @brief ParseLightName returns correct sprite type and name for POINTLIGHT.
 */
TEST(Light, ParseLightName_Point)
{
	auto [sprite, name] = Light::ParseLightName(LightType::POINTLIGHT);
	EXPECT_FALSE(name.empty());
	// SpriteType is forward-declared; verify it's non-zero (not NONE)
	EXPECT_NE(static_cast<int>(sprite), 0);
}

/**
 * @brief ParseLightName returns correct name for SUNLIGHT.
 */
TEST(Light, ParseLightName_Sun)
{
	auto [sprite, name] = Light::ParseLightName(LightType::SUNLIGHT);
	EXPECT_FALSE(name.empty());
	EXPECT_NE(static_cast<int>(sprite), 0);
}

/**
 * @brief ParseLightName returns correct name for SPOTLIGHT.
 */
TEST(Light, ParseLightName_Spot)
{
	auto [sprite, name] = Light::ParseLightName(LightType::SPOTLIGHT);
	EXPECT_FALSE(name.empty());
	EXPECT_NE(static_cast<int>(sprite), 0);
}

/**
 * @brief ParseLightName returns correct name for AREALIGHT.
 */
TEST(Light, ParseLightName_Area)
{
	auto [sprite, name] = Light::ParseLightName(LightType::AREALIGHT);
	EXPECT_FALSE(name.empty());
	EXPECT_NE(static_cast<int>(sprite), 0);
}

/**
 * @brief ParseLightName handles NONELIGHT gracefully.
 */
TEST(Light, ParseLightName_None)
{
	auto [sprite, name] = Light::ParseLightName(LightType::NONELIGHT);
	EXPECT_FALSE(name.empty());
}

// -----------------------------------------------------------------------
// GetTransform override
// -----------------------------------------------------------------------

/**
 * @brief GetTransform returns a valid non-null pointer.
 */
TEST(Light, GetTransform_ReturnsNonNull)
{
	Light light(LightType::POINTLIGHT);
	void* tf = light.GetTransform();
	EXPECT_NE(tf, nullptr);
}

/**
 * @brief GetTransform pointer can be cast back to Transform*.
 */
TEST(Light, GetTransform_CastToTransform)
{
	Light light(LightType::POINTLIGHT);
	void* tf = light.GetTransform();
	Transform* t = static_cast<Transform*>(tf);
	EXPECT_NE(t, nullptr);
	// Verify it's actually a Transform3D
	EXPECT_NE(dynamic_cast<Transform3D*>(t), nullptr);
}

// -----------------------------------------------------------------------
// Light is a scene object (inherits ObjectID)
// -----------------------------------------------------------------------

/**
 * @brief Light has ObjectID identity (unique per instance).
 */
TEST(Light, HasUniqueObjectID)
{
	Light a(LightType::POINTLIGHT);
	Light b(LightType::SUNLIGHT);
	EXPECT_NE(a.GetObjectID(), b.GetObjectID());
}

/**
 * @brief Light object type is GO_LIGHT.
 */
TEST(Light, ObjectTypeIsLight)
{
	Light light(LightType::SPOTLIGHT);
	EXPECT_EQ(light.o_type, ObjectID::GOType::GO_LIGHT);
}

// -----------------------------------------------------------------------
// Shadow parameters are static constexpr
// -----------------------------------------------------------------------

/**
 * @brief Static shadow parameters are accessible and have expected types.
 */
TEST(Light, ShadowParams_AreStaticConstexpr)
{
	// Compile-time checks: values must be accessible
	constexpr float field = Light::sun_shadow_field;
	constexpr float p_near = Light::point_shadow_near;
	constexpr float s_far = Light::spot_shadow_far;
	constexpr float a_range = Light::area_blur_range;

	// Verify they are positive/non-trivial values
	EXPECT_GT(field, 0.0f);
	EXPECT_GT(p_near, 0.0f);
	EXPECT_GT(s_far, 0.0f);
	EXPECT_GT(a_range, 0.0f);
}
