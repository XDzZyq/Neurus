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

TEST(UIDTest, AllocatedIdsAreNonnegativeAndStrictlyIncreasing)
{
	const int before = neurus::UID::GetTotalAllocated();
	int previous = -1;
	for (int index = 0; index < 1000; ++index)
	{
		neurus::UID object;
		EXPECT_GT(object.GetObjectID(), previous) << "allocation " << index;
		previous = object.GetObjectID();
	}
	EXPECT_GE(neurus::UID::GetTotalAllocated(), before + 1000);
}
