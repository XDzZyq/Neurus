/**
 * @file test_cli_parse.cpp
 * @brief CLI validation, exit-code mapping and target resolution tests.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "cli/CliMain.hpp"
#include "cli/CliParse.hpp"
#include "platform/PlatformProcess.hpp"

using namespace neurus::cli;
using namespace neurus::ipc;

namespace
{

/// Sets an environment variable for one test and restores the previous value.
class EnvironmentGuard
{
public:
	EnvironmentGuard(const char* name, const std::string& value)
		: m_name(name)
		, m_previous(neurus::EnvironmentValue(name))
	{
		neurus::SetEnvironmentValue(m_name.c_str(), value.c_str());
	}

	~EnvironmentGuard()
	{
		neurus::SetEnvironmentValue(m_name.c_str(),
		                            m_previous.empty() ? nullptr : m_previous.c_str());
	}

private:
	std::string m_name;
	std::string m_previous;
};

} // namespace

TEST(CliParse, ParseErrorsExitWithUsageCode)
{
	// The documented contract is exit 2 for any usage error; CLI11's own codes
	// (106 for a missing option, 109 for an extra argument) must not leak.
	EXPECT_EQ(RunCli({"neurus", "object", "load"}), static_cast<int>(CliExit::Usage));
	EXPECT_EQ(RunCli({"neurus", "project", "open", "extra.neurus.json"}),
	          static_cast<int>(CliExit::Usage));
	EXPECT_EQ(RunCli({"neurus", "--help"}), static_cast<int>(CliExit::Ok));
}

TEST(CliParse, RejectsInvalidCloseOptions)
{
	CloseOptions options;
	std::string error;
	options.name = "build-1";
	options.all = true;
	EXPECT_FALSE(ValidateCloseOptions(options, error));
	EXPECT_FALSE(error.empty());

	options = {};
	options.save = true;
	options.force = true;
	error.clear();
	EXPECT_FALSE(ValidateCloseOptions(options, error));
	EXPECT_NE(error.find("mutually exclusive"), std::string::npos);

	options = {};
	options.timeoutSeconds = 0;
	EXPECT_FALSE(ValidateCloseOptions(options, error));
}

TEST(CliParse, ObjectGetAcceptsPropertyFlag)
{
	// An empty registry: the get subcommand must accept --position, then fail
	// with the no-session code instead of the usage code.
	const auto directory = std::filesystem::temp_directory_path()
		/ ("neurus-cli-parse-" + std::to_string(neurus::CurrentProcessId()));
	std::error_code code;
	std::filesystem::remove_all(directory, code);

	EnvironmentGuard sessions("NEURUS_SESSIONS_DIR", directory.string());
	EnvironmentGuard terminal("NEURUS_TERMINAL_ID", "unit-test-cli-parse");

	EXPECT_EQ(RunCli({"neurus", "object", "get", "--name", "Mesh", "--position",
	                  "--session", "nosuch"}),
	          static_cast<int>(CliExit::NoSession));
}

TEST(CliParse, OpenRejectsMissingProjectsAndInvalidSessionNames)
{
	OpenOptions options;
	options.project = "definitely/missing.neurus.json";
	std::string error;
	EXPECT_FALSE(ValidateOpenOptions(options, error));
	EXPECT_FALSE(error.empty());
	options = {};
	options.name = "../escape";
	EXPECT_FALSE(ValidateOpenOptions(options, error));
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
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::ScriptSandboxViolation),
	          CliExit::SandboxViolation);
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::AppExited), CliExit::AppMissing);
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::SessionNotFound),
	          CliExit::NoSession);
	EXPECT_EQ(ExitCodeFor(neurus::command::CommandErrorCode::InvalidArgument),
	          CliExit::Usage);
}

TEST(CliParse, ResolvesTargetsForAvailableSessions)
{
	std::vector<SessionRecord> live{SessionRecord{1, "a", 1, 1, "", "", "", 1, ""},
	                                      SessionRecord{1, "b", 2, 2, "", "", "", 1, ""}};
	std::vector<SessionRecord> targets;
	std::vector<std::string> ambiguous;

	EXPECT_EQ(ResolveTargets(std::string("b"), false, live, targets, ambiguous),
	          TargetResolution::Ok);
	ASSERT_EQ(targets.size(), 1u);
	EXPECT_EQ(targets[0].session, "b");

	EXPECT_EQ(ResolveTargets(std::nullopt, false, live, targets, ambiguous),
	          TargetResolution::Ambiguous);
	EXPECT_TRUE(targets.empty());
	ASSERT_EQ(ambiguous.size(), 2u);
	live.pop_back();
	EXPECT_EQ(ResolveTargets(std::nullopt, false, live, targets, ambiguous), TargetResolution::Ok);
	ASSERT_EQ(targets.size(), 1u);
	EXPECT_EQ(targets[0].session, "a");

	for (bool all : {true, false})
	{
		SCOPED_TRACE(all);
		EXPECT_EQ(ResolveTargets(std::nullopt, all, {}, targets, ambiguous), TargetResolution::NoSessions);
		EXPECT_TRUE(targets.empty());
	}
}
