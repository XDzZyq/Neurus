/**
 * @file test_app_args.cpp
 * @brief Parsing tests for the application command-line flags.
 */

#include <gtest/gtest.h>

#include "app/AppArgs.h"

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
