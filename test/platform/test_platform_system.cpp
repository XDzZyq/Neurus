/**
 * @file test_platform_system.cpp
 * @brief Unit tests for the platform-layer system helpers.
 *
 * LocalTime and IsTerminal exist so the logging macros and the timestamp
 * formatters carry no platform `#ifdef`; these tests pin their portable
 * contract. BundleProvidesAppIcon is deliberately not asserted: its expected
 * value is a per-platform policy, so a check would only restate an `#ifdef`.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <ctime>

#include "platform/PlatformSystem.h"

namespace {

TEST(PlatformSystemTest, LocalTimeRoundTripsThroughMktime)
{
	// 2023-11-14T22:13:20Z, clear of the usual DST transition dates.
	const std::time_t fixed = 1700000000;
	std::tm local = neurus::platform::LocalTime(fixed);
	// mktime only agrees with the inverse conversion when tm_isdst keeps the
	// flag LocalTime produced, so it is not forced to -1 here.
	EXPECT_EQ(std::mktime(&local), fixed);
}

TEST(PlatformSystemTest, TemporaryFileIsNotATerminal)
{
	std::FILE* file = std::tmpfile();
	ASSERT_NE(file, nullptr);
	EXPECT_FALSE(neurus::platform::IsTerminal(file));
	std::fclose(file);
}

} // namespace
