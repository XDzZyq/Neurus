/**
 * @file test_cli_parse.cpp
 * @brief CLI validation, exit-code mapping and target resolution tests.
 */

#include <gtest/gtest.h>

#include "cli/CliParse.h"

using namespace neurus::cli;
using namespace neurus::ipc;

TEST(CliParse, CloseRejectsNameWithAll)
{
	CloseOptions options;
	options.name = "build-1";
	options.all = true;
	std::string error;
	EXPECT_FALSE(ValidateCloseOptions(options, error));
	EXPECT_FALSE(error.empty());
}

TEST(CliParse, OpenRejectsMissingProjectFile)
{
	OpenOptions options;
	options.project = "definitely/missing.neurus.json";
	std::string error;
	EXPECT_FALSE(ValidateOpenOptions(options, error));
	EXPECT_FALSE(error.empty());
}

TEST(CliParse, RejectsInvalidSessionName)
{
	OpenOptions options;
	options.name = "../escape";
	std::string error;
	EXPECT_FALSE(ValidateOpenOptions(options, error));
}

TEST(CliParse, RejectsNonPositiveTimeout)
{
	CloseOptions options;
	options.timeoutSeconds = 0;
	std::string error;
	EXPECT_FALSE(ValidateCloseOptions(options, error));
}

TEST(CliParse, ExitCodesMap)
{
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::None), CliExit::Ok);
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::AmbiguousSession),
	          CliExit::NoSession);
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::Timeout), CliExit::Transport);
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::DirtyProject),
	          CliExit::Rejected);
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::ScriptError),
	          CliExit::Rejected);
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::AppExited), CliExit::AppMissing);
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::SessionNotFound),
	          CliExit::NoSession);
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::InvalidArgument),
	          CliExit::Usage);
}

TEST(CliParse, ResolveTargetsByName)
{
	const std::vector<SessionRecord> live{SessionRecord{1, "a", 1, 1, "", "", "", 1, ""},
	                                      SessionRecord{1, "b", 2, 2, "", "", "", 1, ""}};
	std::vector<SessionRecord> targets;
	std::vector<std::string> ambiguous;

	EXPECT_EQ(ResolveTargets(std::string("b"), false, live, targets, ambiguous),
	          TargetResolution::Ok);
	ASSERT_EQ(targets.size(), 1u);
	EXPECT_EQ(targets[0].session, "b");
}

TEST(CliParse, ResolveTargetsAmbiguousWithoutName)
{
	const std::vector<SessionRecord> live{SessionRecord{1, "a", 1, 1, "", "", "", 1, ""},
	                                      SessionRecord{1, "b", 2, 2, "", "", "", 1, ""}};
	std::vector<SessionRecord> targets;
	std::vector<std::string> ambiguous;

	EXPECT_EQ(ResolveTargets(std::nullopt, false, live, targets, ambiguous),
	          TargetResolution::Ambiguous);
	EXPECT_TRUE(targets.empty());
	ASSERT_EQ(ambiguous.size(), 2u);
}

TEST(CliParse, ResolveTargetsSingleWithoutName)
{
	const std::vector<SessionRecord> live{SessionRecord{1, "only", 1, 1, "", "", "", 1, ""}};
	std::vector<SessionRecord> targets;
	std::vector<std::string> ambiguous;

	EXPECT_EQ(ResolveTargets(std::nullopt, false, live, targets, ambiguous),
	          TargetResolution::Ok);
	ASSERT_EQ(targets.size(), 1u);
	EXPECT_EQ(targets[0].session, "only");
}

TEST(CliParse, ResolveTargetsAllAndEmpty)
{
	std::vector<SessionRecord> targets;
	std::vector<std::string> ambiguous;

	EXPECT_EQ(ResolveTargets(std::nullopt, true, {}, targets, ambiguous),
	          TargetResolution::NoSessions);
	EXPECT_EQ(ResolveTargets(std::nullopt, false, {}, targets, ambiguous),
	          TargetResolution::NoSessions);
}
