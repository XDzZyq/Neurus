/**
 * @file test_process_launch.cpp
 * @brief Tests for app lookup, argument building and detached launch helpers.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "cli/ProcessLaunch.h"

using namespace neurus::cli;

namespace
{

#ifdef _WIN32
constexpr const char* kAppName = "Neurus.exe";
constexpr const char* kCliName = "neurus.exe";
#else
constexpr const char* kAppName = "Neurus";
constexpr const char* kCliName = "neurus";
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
