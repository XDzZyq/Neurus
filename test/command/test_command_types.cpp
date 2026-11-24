/**
 * @file test_command_types.cpp
 * @brief Unit tests for command results, error codes and the session catalog.
 */

#include <gtest/gtest.h>

#include "command/CommandDescriptor.hpp"
#include "command/CommandTypes.hpp"

using namespace neurus::command;

TEST(CommandTypes, EveryErrorCodeHasAStableString)
{
	EXPECT_EQ(ToString(CommandErrorCode::Unauthorized), "UNAUTHORIZED");
	EXPECT_EQ(ToString(CommandErrorCode::SessionNotFound), "SESSION_NOT_FOUND");
	EXPECT_EQ(ToString(CommandErrorCode::None), "");
	EXPECT_EQ(ParseErrorCode("DIRTY_PROJECT"), CommandErrorCode::DirtyProject);
	EXPECT_EQ(ParseErrorCode("FRAME_TOO_LARGE"), CommandErrorCode::FrameTooLarge);
	EXPECT_EQ(ToString(CommandErrorCode::ScriptError), "SCRIPT_ERROR");
	EXPECT_EQ(ParseErrorCode("SCRIPT_ERROR"), CommandErrorCode::ScriptError);
	EXPECT_EQ(ToString(CommandErrorCode::ScriptSandboxViolation), "SCRIPT_SANDBOX_VIOLATION");
	EXPECT_EQ(ParseErrorCode("SCRIPT_SANDBOX_VIOLATION"),
	          CommandErrorCode::ScriptSandboxViolation);
	EXPECT_FALSE(ParseErrorCode("NOT_A_CODE").has_value());
}

TEST(CommandTypes, CatalogHasNoDuplicatesAndMarksSensitive)
{
	auto catalog = MakeCommandCatalog();
	EXPECT_NE(catalog.Find("session.status"), nullptr);
	EXPECT_NE(catalog.Find("project.save"), nullptr);
	EXPECT_NE(catalog.Find("object.set"), nullptr);

	const auto* close = catalog.Find("session.close");
	ASSERT_NE(close, nullptr);
	EXPECT_TRUE(close->sensitive);
	EXPECT_TRUE(close->mutating);

	const auto* open = catalog.Find("project.open");
	ASSERT_NE(open, nullptr);
	EXPECT_TRUE(open->mutating);
	EXPECT_EQ(open->requiredArgs, std::vector<std::string>{"path"});

	EXPECT_EQ(catalog.Find("session.unknown"), nullptr);
}
