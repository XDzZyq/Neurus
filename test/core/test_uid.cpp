#include <gtest/gtest.h>
#include "core/UID.hpp"

TEST(UIDTest, StandaloneObjectHasNoPoolIdentity)
{
	neurus::UID object;
	EXPECT_FALSE(object.GetObjectID());
	EXPECT_THROW(object.RequireObjectID(), std::logic_error);
}
