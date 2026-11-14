/**
 * @file test_process_launch.cpp
 * @brief Tests for app lookup, argument building and detached launch helpers.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "cli/AppArguments.h"
#include "platform/PlatformProcess.h"

using namespace neurus;
using namespace neurus::cli;

namespace
{

#ifdef _WIN32
constexpr const char* kAppName = "Neurus.exe";
constexpr const char* kCliName = "neurus.exe";
constexpr const char* kExitingProgram = "C:/Windows/System32/cmd.exe";
const std::vector<std::string> kExitingArguments = {"/c", "exit"};
#elif defined(__linux__)
// Linux ships the headless renderer, resolved as a sibling of the CLI.
constexpr const char* kAppName = "neurus_headless";
constexpr const char* kCliName = "neurus";
constexpr const char* kExitingProgram = "/usr/bin/true";
const std::vector<std::string> kExitingArguments = {};
#else
constexpr const char* kAppName = "Neurus";
constexpr const char* kCliName = "neurus";
constexpr const char* kExitingProgram = "/usr/bin/true";
const std::vector<std::string> kExitingArguments = {};
#endif

class ProcessLaunchTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_root = std::filesystem::temp_directory_path()
			/ ("neurus-launch-" + std::to_string(std::hash<std::string>{}(
				::testing::UnitTest::GetInstance()->current_test_info()->name())));
		std::filesystem::remove_all(m_root);
		std::filesystem::create_directories(m_root / "cli");
	}

	void TearDown() override
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_root, ignored);
	}

	std::filesystem::path Touch(const std::filesystem::path& path)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path).put('x');
		return path;
	}

	std::filesystem::path m_root;
};

} // namespace

TEST_F(ProcessLaunchTest, ExplicitOverrideWins)
{
	const auto overridePath = Touch(m_root / "custom-app");
	const auto resolved = ResolveAppExecutable(m_root / "cli" / kCliName, overridePath, "");
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(*resolved, overridePath);
}

TEST_F(ProcessLaunchTest, FindsAppBesideTheCliDirectory)
{
	Touch(m_root / kAppName);
	const auto resolved = ResolveAppExecutable(m_root / "cli" / kCliName, std::nullopt, "");
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(resolved->filename(), kAppName);
}

TEST_F(ProcessLaunchTest, EnvironmentPathWinsOverSearch)
{
	const auto envPath = Touch(m_root / "from-env");
	const auto resolved = ResolveAppExecutable(m_root / "cli" / kCliName, std::nullopt,
	                                           envPath.string());
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(*resolved, envPath);
}

TEST_F(ProcessLaunchTest, MissingAppReturnsNullopt)
{
	EXPECT_FALSE(ResolveAppExecutable(m_root / "cli" / kCliName, std::nullopt, "").has_value());
}

TEST_F(ProcessLaunchTest, BuildsSessionArguments)
{
	const auto arguments = BuildAppArguments("build-1", std::string("C:/p.neurus.json"));
	ASSERT_EQ(arguments.size(), 4u);
	EXPECT_EQ(arguments[0], "--session");
	EXPECT_EQ(arguments[1], "build-1");
	EXPECT_EQ(arguments[2], "--project");
	EXPECT_EQ(arguments[3], "C:/p.neurus.json");

	const auto withoutProject = BuildAppArguments("build-1", std::nullopt);
	ASSERT_EQ(withoutProject.size(), 2u);

	// Preloads are forwarded as repeated option/value pairs.
	const auto withPreload =
		BuildAppArguments("build-1", std::nullopt, {"json", "numpy"});
	ASSERT_EQ(withPreload.size(), 6u);
	EXPECT_EQ(withPreload[0], "--session");
	EXPECT_EQ(withPreload[1], "build-1");
	EXPECT_EQ(withPreload[2], "--preload");
	EXPECT_EQ(withPreload[3], "json");
	EXPECT_EQ(withPreload[4], "--preload");
	EXPECT_EQ(withPreload[5], "numpy");
}

TEST_F(ProcessLaunchTest, SpawningANonExecutableFails)
{
	// `open` must report a launch that never started instead of polling a
	// process that is already gone.
	const auto notAnApp = Touch(m_root / "not-an-app");
	std::string error;
	const auto pid = SpawnDetached(notAnApp, {}, m_root / "launch.log", error);
	EXPECT_FALSE(pid.has_value());
	EXPECT_FALSE(error.empty());
}

TEST_F(ProcessLaunchTest, ExitedChildIsNotAlive)
{
	// `open` polls IsProcessAlive() to notice an app that died during startup;
	// an exited child must stop counting as alive even before anyone reaps it.
	std::string error;
	const auto pid = SpawnDetached(kExitingProgram, kExitingArguments,
	                               m_root / "launch.log", error);
	ASSERT_TRUE(pid.has_value()) << error;

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (IsProcessAlive(*pid) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_FALSE(IsProcessAlive(*pid));
}

#ifdef __APPLE__
TEST_F(ProcessLaunchTest, FindsTheApplicationBundle)
{
	Touch(m_root / "Neurus.app" / "Contents" / "MacOS" / "Neurus");
	const auto resolved = ResolveAppExecutable(m_root / "cli" / kCliName, std::nullopt, "");
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(resolved->filename(), "Neurus");
	EXPECT_NE(resolved->string().find("Neurus.app"), std::string::npos);
}
#endif
