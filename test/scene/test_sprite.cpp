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

TEST(SpriteTest, DefaultsToAnUntexturedSprite)
{
	Sprite sprite;

	EXPECT_EQ(sprite.o_type, ObjectID::GOType::GO_SPRITE);

	EXPECT_FLOAT_EQ(sprite.spr_opacity, 0.9f);

	EXPECT_EQ(sprite.spr_type, SpriteType::NONE_SPRITE);

	EXPECT_EQ(sprite.spr_tex, nullptr);
	EXPECT_TRUE(sprite.ParsePath().empty());
}

// -----------------------------------------------------------------------
// Sprite - ParsePath type-to-path mapping
// -----------------------------------------------------------------------

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
		SCOPED_TRACE(static_cast<int>(types[i]));
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
