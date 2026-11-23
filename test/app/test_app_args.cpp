/**
 * @file test_app_args.cpp
 * @brief Parsing tests for the application command-line flags.
 */

#include <gtest/gtest.h>

#include "app/AppArgs.hpp"

#include <string>
#include <vector>

using namespace neurus;

TEST(AppArgs, UnrecognizedFlagsLeaveDefaultsInPlace)
{
	char arg0[] = "Neurus";
	char arg1[] = "--some-qt-flag";
	char arg2[] = "value";
	char* argv[] = {arg0, arg1, arg2};

	const auto args = AppArgs::Parse(3, argv);
	EXPECT_TRUE(args.error.empty());
	EXPECT_FALSE(args.session.has_value());
	EXPECT_FALSE(args.project.has_value());
	EXPECT_EQ(args.width, 1280u);
	EXPECT_EQ(args.height, 720u);
}

TEST(AppArgs, ParsesSeparateAndJoinedFlagValues)
{
	const std::vector<std::string> forms[] = {
		{"Neurus", "--session", "build-1", "--project", "C:/work/p.neurus.json", "--width", "1920", "--height=1080"},
		{"Neurus", "--session=build-1", "--project=C:/work/p.neurus.json", "--width=1920", "--height", "1080"}
	};
	for (const auto& arguments : forms)
	{
		SCOPED_TRACE(arguments[1]);
		const auto args = AppArgs::Parse(arguments);
		EXPECT_TRUE(args.error.empty());
		EXPECT_EQ(args.width, 1920u);
		EXPECT_EQ(args.height, 1080u);
		EXPECT_EQ(args.session, "build-1");
		EXPECT_EQ(args.project, "C:/work/p.neurus.json");
	}
}

TEST(AppArgs, ParsesRepeatedAndCommaSeparatedPreloads)
{
	const std::vector<std::string> arguments{
		"Neurus", "--preload", "json", "--preload=numpy,torch", "--preload=scipy"};

	const auto args = AppArgs::Parse(arguments);
	EXPECT_TRUE(args.error.empty());
	ASSERT_EQ(args.preload.size(), 4u);
	EXPECT_EQ(args.preload[0], "json");
	EXPECT_EQ(args.preload[1], "numpy");
	EXPECT_EQ(args.preload[2], "torch");
	EXPECT_EQ(args.preload[3], "scipy");
}

TEST(AppArgs, PreloadWithoutAValueIsAnError)
{
	const std::vector<std::string> arguments{"Neurus", "--preload"};

	const auto args = AppArgs::Parse(arguments);
	EXPECT_FALSE(args.error.empty());
	EXPECT_TRUE(args.preload.empty());
}

TEST(AppArgs, RejectsMalformedExtents)
{
	// A numeric prefix, a negative value, zero, an out-of-range value and an
	// empty token must all be rejected: std::stoul accepted the first, wrapped
	// the second, and the renderer would have used the result as its extent.
	for (const char* bad : {"1280px", "-1", "0", "99999", ""})
	{
		const std::vector<std::string> arguments{"Neurus", "--width", bad};
		const auto args = AppArgs::Parse(arguments);
		EXPECT_FALSE(args.error.empty()) << "accepted --width '" << bad << "'";
		EXPECT_EQ(args.width, 1280u) << "a rejected token must not change the value";
	}
}
