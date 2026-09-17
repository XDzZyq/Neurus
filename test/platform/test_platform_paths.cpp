/**
 * @file test_platform_paths.cpp
 * @brief Unit tests for the Platform layer's HomeDirectory() helper.
 *
 * Verifies the QDir::homePath()-equivalent contract: a non-empty absolute
 * path that follows $HOME (POSIX) / %USERPROFILE% (Windows), plus the
 * Preferences integration (the ~/.neurus path is built on top of it).
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "app/Preferences.h"
#include "platform/PlatformPaths.h"

using namespace neurus;

namespace {

#ifdef _WIN32
constexpr const char* kHomeEnv = "USERPROFILE";
#else
constexpr const char* kHomeEnv = "HOME";
#endif

TEST(PlatformPathsTest, HomeDirectoryIsNonEmptyAndAbsolute)
{
	const std::filesystem::path home = HomeDirectory();
	EXPECT_FALSE(home.empty());
	EXPECT_TRUE(home.is_absolute());
}

TEST(PlatformPathsTest, HomeDirectoryFollowsHomeEnvVar)
{
	const char* env = std::getenv(kHomeEnv);
	if (!env || !*env)
		return;  // Unset env — fallback path, nothing to compare against.

	EXPECT_EQ(HomeDirectory().string(), std::string(env));
}

TEST(PlatformPathsTest, PreferencesDefaultPathBuiltOnHomeDirectory)
{
	const std::filesystem::path expected =
		HomeDirectory() / ".neurus" / "preferences.json";
	EXPECT_EQ(Preferences::DefaultPath(), expected.generic_string());
}

} // namespace
