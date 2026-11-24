/**
 * @file test_light.cpp
 * @brief Unit tests for Light class (point, sun, spot, area).
 *
 * TDD: RED (test written first) → GREEN (implementation verified).
 * All tests are pure CPU math - no GPU required.
 */

#include <gtest/gtest.h>

#include "scene/Light.hpp"
#include "scene/Sprite.hpp"

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
	EXPECT_EQ(light.GetTransform(), static_cast<Transform3D*>(&light));
}

TEST(Light, TypedLightsStartCleanWithCommonDefaults)
{
	int previousUid = -1;
	for (const auto type : {LightType::POINTLIGHT, LightType::SUNLIGHT,
	                        LightType::SPOTLIGHT, LightType::AREALIGHT})
	{
		SCOPED_TRACE(static_cast<int>(type));
		Light light(type);
		EXPECT_GT(light.GetObjectID(), previousUid);
		previousUid = light.GetObjectID();
		EXPECT_TRUE(light.use_shadow);
		EXPECT_EQ(light.light_type, type);
		EXPECT_EQ(light.o_type, ObjectID::GOType::GO_LIGHT);
		EXPECT_FLOAT_EQ(light.light_power, 10.0f);
		EXPECT_EQ(light.light_color, glm::vec3(1.0f));
		EXPECT_FALSE(light.is_light_changed);
	}
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

TEST(Light, ChangingEmissionMarksLightDirty)
{
	Light light(LightType::POINTLIGHT);

	light.is_light_changed = false;

	glm::vec3 red(1.0f, 0.0f, 0.0f);
	light.SetColor(red);

	EXPECT_EQ(light.light_color, red);
	EXPECT_TRUE(light.is_light_changed);

	light.is_light_changed = false;

	light.SetPower(25.0f);

	EXPECT_FLOAT_EQ(light.light_power, 25.0f);
	EXPECT_TRUE(light.is_light_changed);
}

TEST(Light, ShadowToggleMarksLightDirty)
{
	Light light(LightType::POINTLIGHT);
	for (const bool enabled : {false, true})
	{
		SCOPED_TRACE(enabled);
		light.is_light_changed = false;
		light.SetShadow(enabled);
		EXPECT_EQ(light.use_shadow, enabled);
		EXPECT_TRUE(light.is_light_changed);
	}
}

TEST(Light, SetRadius)
{
	Light light(LightType::POINTLIGHT);
	EXPECT_FLOAT_EQ(light.light_radius, 0.01f);
	light.is_light_changed = false;

	light.SetRadius(0.1f);

	EXPECT_FLOAT_EQ(light.light_radius, 0.1f);
	EXPECT_TRUE(light.is_light_changed);
}

TEST(Light, ChangingEitherSpotCutoffMarksLightDirty)
{
	Light light(LightType::SPOTLIGHT);
	EXPECT_FLOAT_EQ(light.spot_cutoff, 0.9f);
	EXPECT_FLOAT_EQ(light.spot_outer_cutoff, 0.8f);

	light.is_light_changed = false;

	light.SetCutoff(0.85f);

	EXPECT_FLOAT_EQ(light.spot_cutoff, 0.85f);
	EXPECT_TRUE(light.is_light_changed);

	light.is_light_changed = false;

	light.SetOuterCutoff(0.75f);

	EXPECT_FLOAT_EQ(light.spot_outer_cutoff, 0.75f);
	EXPECT_TRUE(light.is_light_changed);
}

TEST(Light, SetRatio)
{
	Light light(LightType::AREALIGHT);
	EXPECT_FLOAT_EQ(light.area_ratio, 1.0f);
	light.is_light_changed = false;

	light.SetRatio(2.0f);

	EXPECT_FLOAT_EQ(light.area_ratio, 2.0f);
	EXPECT_TRUE(light.is_light_changed);
}

TEST(Light, MapsEachLightToItsDisplayNameAndMarker)
{
	struct Mapping
	{
		LightType type;
		const char* name;
		SpriteType marker;
	};
	const Mapping mappings[] = {
		{LightType::POINTLIGHT, "Point Light", SpriteType::POINT_LIGHT_SPRITE},
		{LightType::SUNLIGHT, "Sun Light", SpriteType::SUN_LIGHT_SPRITE},
		{LightType::SPOTLIGHT, "Spot Light", SpriteType::SPOT_LIGHT_SPRITE},
		{LightType::NONELIGHT, "None", SpriteType::NONE_SPRITE}
	};
	for (const auto& mapping : mappings)
	{
		SCOPED_TRACE(mapping.name);
		const auto [marker, name] = Light::ParseLightName(mapping.type);
		EXPECT_EQ(name, mapping.name);
		EXPECT_EQ(marker, mapping.marker);
	}
	// There is no dedicated area-light sprite. Any visible marker satisfies the contract.
	const auto [areaMarker, areaName] = Light::ParseLightName(LightType::AREALIGHT);
	EXPECT_EQ(areaName, "Area Light");
	EXPECT_NE(areaMarker, SpriteType::NONE_SPRITE);
}

// -----------------------------------------------------------------------
// Shadow parameters are static constexpr
// -----------------------------------------------------------------------

static_assert(Light::sun_shadow_field > 0.0f);
static_assert(Light::point_shadow_near > 0.0f);
static_assert(Light::spot_shadow_far > 0.0f);
static_assert(Light::area_blur_range > 0.0f);
