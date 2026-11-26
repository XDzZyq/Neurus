#include <gtest/gtest.h>
#include "core/Log.hpp"
#include "core/UID.hpp"

TEST(UIDTest, StandaloneObjectHasNoPoolIdentity)
{
	neurus::UID object;
	auto& log = neurus::LogBuffer::instance();
	const auto next = log.HeadSeq() + 1;
	EXPECT_EQ(object.GetObjectID(), -1);
	neurus::LogEntry entry{};
	ASSERT_TRUE(log.BySeq(next, entry));
	EXPECT_EQ(entry.level, neurus::LogLevel::Error);
}
