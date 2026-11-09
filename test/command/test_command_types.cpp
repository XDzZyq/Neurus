/**
 * @file test_command_types.cpp
 * @brief Unit tests for command results, error codes and the session catalog.
 */

#include <gtest/gtest.h>

#include "command/CommandDescriptor.h"
#include "command/CommandTypes.h"

using namespace neurus::command;

TEST(CommandTypes, SuccessHasNoError)
{
	auto result = CommandResult::Success({{"session", "build-1"}});
	EXPECT_TRUE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::None);
	EXPECT_EQ(result.details["session"], "build-1");
}

TEST(CommandTypes, FailureCarriesCodeAndMessage)
{
	auto result = CommandResult::Failure(CommandErrorCode::DirtyProject, "unsaved changes");
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(ToString(result.code), "DIRTY_PROJECT");
	EXPECT_EQ(result.message, "unsaved changes");
}

TEST(CommandTypes, EveryErrorCodeHasAStableString)
{
	EXPECT_EQ(ToString(CommandErrorCode::Unauthorized), "UNAUTHORIZED");
	EXPECT_EQ(ToString(CommandErrorCode::SessionNotFound), "SESSION_NOT_FOUND");
	EXPECT_EQ(ToString(CommandErrorCode::None), "");
	EXPECT_EQ(ParseErrorCode("DIRTY_PROJECT"), CommandErrorCode::DirtyProject);
	EXPECT_EQ(ParseErrorCode("FRAME_TOO_LARGE"), CommandErrorCode::FrameTooLarge);
	EXPECT_FALSE(ParseErrorCode("NOT_A_CODE").has_value());
}

TEST(CommandTypes, SessionCatalogHasNoDuplicatesAndMarksSensitive)
{
	auto catalog = MakeSessionCatalog();
	ASSERT_EQ(catalog.All().size(), 2u);
	EXPECT_NE(catalog.Find("session.status"), nullptr);

	const auto* close = catalog.Find("session.close");
	ASSERT_NE(close, nullptr);
	EXPECT_TRUE(close->sensitive);
	EXPECT_TRUE(close->mutating);

	EXPECT_EQ(catalog.Find("session.unknown"), nullptr);
}
