/**
 * @file test_terminal_binding.cpp
 * @brief Per-terminal default-session binding tests.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "cli/SessionClient.h"
#include "cli/TerminalBinding.h"
#include "platform/PlatformProcess.h"

#include <vector>

using namespace neurus;
using namespace neurus::cli;

namespace
{

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

TEST(TerminalBinding, WritesReadsAndClearsTheBinding)
{
	const auto directory = std::filesystem::temp_directory_path()
		/ ("neurus-terminal-binding-" + std::to_string(neurus::CurrentProcessId()));
	std::error_code error;
	std::filesystem::remove_all(directory, error);

	EnvironmentGuard sessions("NEURUS_SESSIONS_DIR", directory.string());
	EnvironmentGuard terminal("NEURUS_TERMINAL_ID", "unit-test-terminal");

	EXPECT_FALSE(neurus::cli::TerminalIdentity().empty());
	EXPECT_FALSE(ReadTerminalBinding().has_value());

	std::string message;
	ASSERT_TRUE(WriteTerminalBinding("dev-1", message)) << message;
	auto bound = ReadTerminalBinding();
	ASSERT_TRUE(bound.has_value());
	EXPECT_EQ(*bound, "dev-1");
	EXPECT_TRUE(std::filesystem::exists(TerminalBindingPath()));

	// A different session leaves this terminal's binding alone.
	ClearTerminalBindingIfMatches("other");
	bound = ReadTerminalBinding();
	ASSERT_TRUE(bound.has_value());
	EXPECT_EQ(*bound, "dev-1");

	ClearTerminalBindingIfMatches("dev-1");
	EXPECT_FALSE(ReadTerminalBinding().has_value());

	std::filesystem::remove_all(directory, error);
}

TEST(TerminalBinding, IdentityOverrideIsStable)
{
	EnvironmentGuard terminal("NEURUS_TERMINAL_ID", "parallel-agent-7");
	EXPECT_EQ(neurus::cli::TerminalIdentity(), "explicit:parallel-agent-7");
	EXPECT_EQ(neurus::cli::TerminalIdentity(), "explicit:parallel-agent-7");
}

TEST(TerminalBinding, ResolutionPrefersExplicitThenEnvironmentThenBinding)
{
	const auto directory = std::filesystem::temp_directory_path()
		/ ("neurus-terminal-resolution-" + std::to_string(neurus::CurrentProcessId()));
	std::error_code error;
	std::filesystem::remove_all(directory, error);

	EnvironmentGuard sessions("NEURUS_SESSIONS_DIR", directory.string());
	EnvironmentGuard terminal("NEURUS_TERMINAL_ID", "resolution-terminal");
	EnvironmentGuard environment("NEURUS_SESSION", "");

	const std::vector<ipc::SessionRecord> live{
		ipc::SessionRecord{1, "dev-1", 1, 1, "", "", "", 1, ""},
		ipc::SessionRecord{1, "dev-2", 2, 2, "", "", "", 1, ""},
	};

	EXPECT_EQ(ResolveSessionNameForCommand(std::string("explicit"), live), "explicit");

	neurus::SetEnvironmentValue("NEURUS_SESSION", "dev-2");
	EXPECT_EQ(ResolveSessionNameForCommand(std::nullopt, live), "dev-2");
	neurus::SetEnvironmentValue("NEURUS_SESSION", nullptr);

	std::string message;
	ASSERT_TRUE(WriteTerminalBinding("dev-1", message)) << message;
	EXPECT_EQ(ResolveSessionNameForCommand(std::nullopt, live), "dev-1");

	// A binding whose session is gone is cleared and falls back to none.
	const std::vector<ipc::SessionRecord> onlyDev2{live[1]};
	EXPECT_FALSE(ResolveSessionNameForCommand(std::nullopt, onlyDev2).has_value());
	EXPECT_FALSE(ReadTerminalBinding().has_value());

	std::filesystem::remove_all(directory, error);
}
