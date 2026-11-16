/**
 * @file test_sprite.cpp
 * @brief Unit tests for Sprite class (billboard icon data object).
 *
 * TDD: RED (test written first) -> GREEN (implementation verified).
 * No GPU required - all tests are pure CPU data tests.
 */

#include <gtest/gtest.h>

#include "scene/Sprite.hpp"

using namespace neurus;

// -----------------------------------------------------------------------
// Sprite - inheritance & identity
// -----------------------------------------------------------------------

/**
 * @brief Sprite inherits from ObjectID and gets a unique UID.
 */
TEST(SpriteTest, InheritsObjectID)
{
	Sprite sprite;
	EXPECT_GE(sprite.GetObjectID(), 0);
}

/**
 * @brief Sprite sets its type to GO_SPRITE at construction.
 */
TEST(SpriteTest, DefaultTypeIsGoSprite)
{
	Sprite sprite;
	EXPECT_EQ(sprite.o_type, ObjectID::GOType::GO_SPRITE);
}

// -----------------------------------------------------------------------
// Sprite - default member values
// -----------------------------------------------------------------------

/**
 * @brief Default opacity is 0.9f.
 */
TEST(SpriteTest, DefaultOpacity)
{
	Sprite sprite;
	EXPECT_FLOAT_EQ(sprite.spr_opacity, 0.9f);
}

/**
 * @brief Default sprite type is NONE_SPRITE.
 */
TEST(SpriteTest, DefaultTypeIsNone)
{
	Sprite sprite;
	EXPECT_EQ(sprite.spr_type, SpriteType::NONE_SPRITE);
}

/**
 * @brief Default texture pointer is null.
 */
TEST(SpriteTest, DefaultTextureIsNull)
{
	Sprite sprite;
	EXPECT_EQ(sprite.spr_tex, nullptr);
}

// -----------------------------------------------------------------------
// Sprite - ParsePath type-to-path mapping
// -----------------------------------------------------------------------

/**
 * @brief NONE_SPRITE returns empty string.
 */
TEST(SpriteTest, ParsePathNoneReturnsEmpty)
{
	Sprite sprite;
	sprite.spr_type = SpriteType::NONE_SPRITE;
	EXPECT_TRUE(sprite.ParsePath().empty());
}

/**
 * @brief POINT_LIGHT_SPRITE returns non-empty icon path.
 */
TEST(SpriteTest, ParsePathPointLight)
{
	Sprite sprite;
	sprite.spr_type = SpriteType::POINT_LIGHT_SPRITE;
	EXPECT_FALSE(sprite.ParsePath().empty());
}

/**
 * @brief SUN_LIGHT_SPRITE returns non-empty icon path.
 */
TEST(SpriteTest, ParsePathSunLight)
{
	Sprite sprite;
	sprite.spr_type = SpriteType::SUN_LIGHT_SPRITE;
	EXPECT_FALSE(sprite.ParsePath().empty());
}

/**
 * @brief SPOT_LIGHT_SPRITE returns non-empty icon path.
 */
TEST(SpriteTest, ParsePathSpotLight)
{
	Sprite sprite;
	sprite.spr_type = SpriteType::SPOT_LIGHT_SPRITE;
	EXPECT_FALSE(sprite.ParsePath().empty());
}

/**
 * @brief CAM_SPRITE returns non-empty icon path.
 */
TEST(SpriteTest, ParsePathCam)
{
	Sprite sprite;
	sprite.spr_type = SpriteType::CAM_SPRITE;
	EXPECT_FALSE(sprite.ParsePath().empty());
}

/**
 * @brief ENVIRN_SPRITE returns non-empty icon path.
 */
TEST(SpriteTest, ParsePathEnvirn)
{
	Sprite sprite;
	sprite.spr_type = SpriteType::ENVIRN_SPRITE;
	EXPECT_FALSE(sprite.ParsePath().empty());
}

/**
 * @brief Each sprite type returns a distinct path.
 */
TEST(SpriteTest, ParsePathDistinctPerType)
{
	Sprite sprite;

	// Collect paths for all types
	std::string paths[5];
	SpriteType types[5] = {
		SpriteType::POINT_LIGHT_SPRITE,
		SpriteType::SUN_LIGHT_SPRITE,
		SpriteType::SPOT_LIGHT_SPRITE,
		SpriteType::CAM_SPRITE,
		SpriteType::ENVIRN_SPRITE
	};

	for (int i = 0; i < 5; ++i)
	{
		sprite.spr_type = types[i];
		paths[i] = sprite.ParsePath();
		EXPECT_FALSE(paths[i].empty());
	}

	// Verify all paths are distinct
	for (int i = 0; i < 5; ++i)
	{
		for (int j = i + 1; j < 5; ++j)
		{
			EXPECT_NE(paths[i], paths[j])
				<< "Types " << static_cast<int>(types[i])
				<< " and " << static_cast<int>(types[j])
				<< " return the same path";
		}
	}
}
