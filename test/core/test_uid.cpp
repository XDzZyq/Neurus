/**
 * @file test_uid.cpp
 * @brief Tests for the core UID identity primitive.
 *
 * Covers:
 * - Unique ID generation across 1000 instances
 * - Counter increments correctly
 * - GetObjectID() returns assigned ID
 * - GetTotalAllocated() returns global count
 */

#include <gtest/gtest.h>
#include <core/UID.hpp>

// --- UID Tests -----------------------------------------------------------

/**
 * @test UIDs are unique across multiple instances.
 */
TEST(UIDTest, UniqueIDs)
{
	constexpr int kCount = 1000;
	int ids[kCount];

	for (int i = 0; i < kCount; ++i)
	{
		neurus::UID uid;
		ids[i] = uid.GetObjectID();
	}

	// Verify all IDs are unique
	for (int i = 0; i < kCount; ++i)
	{
		for (int j = i + 1; j < kCount; ++j)
		{
			EXPECT_NE(ids[i], ids[j]) << "IDs [" << i << "] and [" << j << "] collide";
		}
	}
}

/**
 * @test GetTotalAllocated returns non-zero after creating UIDs.
 */
TEST(UIDTest, TotalAllocatedIncrements)
{
	int before = neurus::UID::GetTotalAllocated();

	{
		neurus::UID a;
		neurus::UID b;
		neurus::UID c;
		(void)a;
		(void)b;
		(void)c;
	}

	int afterScope = neurus::UID::GetTotalAllocated();
	EXPECT_GE(afterScope, before + 3);
}

/**
 * @test UID GetObjectID returns a valid (non-negative) ID.
 */
TEST(UIDTest, GetObjectIDValid)
{
	neurus::UID uid;
	EXPECT_GE(uid.GetObjectID(), 0);
}

/**
 * @test Two consecutive UIDs have increasing IDs.
 */
TEST(UIDTest, IDsIncrease)
{
	neurus::UID first;
	neurus::UID second;
	EXPECT_LT(first.GetObjectID(), second.GetObjectID());
}
