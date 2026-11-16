/**
 * @file test_app_args.cpp
 * @brief Parsing tests for the application command-line flags.
 */

#include <gtest/gtest.h>

#include "app/AppArgs.hpp"

#include <string>
#include <vector>

using namespace neurus;

TEST(AppArgs, ParsesSessionAndProject)
{
	char arg0[] = "Neurus";
	char arg1[] = "--session";
	char arg2[] = "build-1";
	char arg3[] = "--project";
	char arg4[] = "C:/work/p.neurus.json";
	char* argv[] = {arg0, arg1, arg2, arg3, arg4};

	const auto args = AppArgs::Parse(5, argv);
	EXPECT_TRUE(args.error.empty());
	ASSERT_TRUE(args.session.has_value());
	EXPECT_EQ(*args.session, "build-1");
	ASSERT_TRUE(args.project.has_value());
	EXPECT_EQ(*args.project, "C:/work/p.neurus.json");
}

TEST(AppArgs, AbsentFlagsAreEmpty)
{
	char arg0[] = "Neurus";
	char* argv[] = {arg0};

	const auto args = AppArgs::Parse(1, argv);
	EXPECT_TRUE(args.error.empty());
	EXPECT_FALSE(args.session.has_value());
	EXPECT_FALSE(args.project.has_value());
}

TEST(AppArgs, UnknownArgumentsDoNotFail)
{
	char arg0[] = "Neurus";
	char arg1[] = "--some-qt-flag";
	char arg2[] = "value";
	char* argv[] = {arg0, arg1, arg2};

	const auto args = AppArgs::Parse(3, argv);
	EXPECT_TRUE(args.error.empty());
	EXPECT_FALSE(args.session.has_value());
}

TEST(AppArgs, ParsesEqualsForm)
{
	const std::vector<std::string> arguments{
		"Neurus", "--session=build-1", "--project=C:/work/p.neurus.json"};

	const auto args = AppArgs::Parse(arguments);
	EXPECT_TRUE(args.error.empty());
	ASSERT_TRUE(args.session.has_value());
	EXPECT_EQ(*args.session, "build-1");
	ASSERT_TRUE(args.project.has_value());
	EXPECT_EQ(*args.project, "C:/work/p.neurus.json");
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

TEST(AppArgs, ParsesWidthAndHeight)
{
	const std::vector<std::string> arguments{
		"Neurus", "--width", "1920", "--height=1080"};

	const auto args = AppArgs::Parse(arguments);
	EXPECT_TRUE(args.error.empty());
	EXPECT_EQ(args.width, 1920u);
	EXPECT_EQ(args.height, 1080u);
}

TEST(AppArgs, DefaultExtentIs720p)
{
	const auto args = AppArgs::Parse(std::vector<std::string>{"Neurus"});
	EXPECT_EQ(args.width, 1280u);
	EXPECT_EQ(args.height, 720u);
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
