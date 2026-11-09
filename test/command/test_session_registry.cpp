/**
 * @file test_session_registry.cpp
 * @brief Registry, claim and name-validation tests.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "ipc/RandomToken.h"
#include "ipc/SessionRegistry.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace neurus::ipc;

namespace
{

int CurrentPid()
{
#ifdef _WIN32
	return _getpid();
#else
	return static_cast<int>(::getpid());
#endif
}

SessionRecord MakeRecord(std::string name, int pid, std::uint16_t port = 51234)
{
	SessionRecord record;
	record.session = std::move(name);
	record.pid = pid;
	record.port = port;
	record.token = std::string(64, 'a');
	record.project = "proj.neurus.json";
	record.appVersion = "0.1.0";
	record.protocolVersion = 1;
	record.startedAt = "2026-11-09T00:00:00Z";
	return record;
}

class SessionRegistryTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_directory = std::filesystem::temp_directory_path()
			/ ("neurus-registry-" + GenerateToken().substr(0, 8));
		std::filesystem::create_directories(m_directory);
	}

	void TearDown() override
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_directory, ignored);
	}

	std::filesystem::path m_directory;
};

} // namespace

TEST_F(SessionRegistryTest, WriteReadRemove)
{
	SessionRegistry registry(m_directory);
	std::string error;
	ASSERT_TRUE(registry.Write(MakeRecord("build-1", CurrentPid()), error)) << error;

	auto loaded = registry.Read("build-1");
	ASSERT_TRUE(loaded.has_value());
	EXPECT_EQ(loaded->port, 51234);
	EXPECT_EQ(loaded->project, "proj.neurus.json");
	EXPECT_EQ(loaded->token, std::string(64, 'a'));

	EXPECT_TRUE(registry.Remove("build-1"));
	EXPECT_FALSE(registry.Read("build-1").has_value());
}

TEST_F(SessionRegistryTest, WriteLeavesNoTemporaryFile)
{
	SessionRegistry registry(m_directory);
	std::string error;
	ASSERT_TRUE(registry.Write(MakeRecord("build-1", CurrentPid()), error)) << error;
	EXPECT_FALSE(std::filesystem::exists(m_directory / "build-1.json.tmp"));
}

TEST(SessionNameValidation, RejectsTraversalAndSeparators)
{
	EXPECT_TRUE(IsValidSessionName("build-1"));
	EXPECT_TRUE(IsValidSessionName("A_9"));
	EXPECT_FALSE(IsValidSessionName(""));
	EXPECT_FALSE(IsValidSessionName("../escape"));
	EXPECT_FALSE(IsValidSessionName("a/b"));
	EXPECT_FALSE(IsValidSessionName("a\\b"));
	EXPECT_FALSE(IsValidSessionName("-leading"));
	EXPECT_FALSE(IsValidSessionName(std::string(65, 'a')));
}

TEST_F(SessionRegistryTest, InvalidNameTouchesNothing)
{
	SessionRegistry registry(m_directory);
	std::string error;
	EXPECT_FALSE(registry.Write(MakeRecord("../escape", CurrentPid()), error));
	EXPECT_FALSE(std::filesystem::exists(m_directory.parent_path() / "escape.json"));
}

TEST_F(SessionRegistryTest, ListLivePrunesDeadPid)
{
	SessionRegistry registry(m_directory);
	std::string error;
	ASSERT_TRUE(registry.Write(MakeRecord("dead", 0, 1), error)) << error;
	ASSERT_TRUE(registry.Write(MakeRecord("alive", CurrentPid(), 2), error)) << error;

	const auto live = registry.ListLive();
	ASSERT_EQ(live.size(), 1u);
	EXPECT_EQ(live[0].session, "alive");
	EXPECT_FALSE(registry.Read("dead").has_value());
}

TEST_F(SessionRegistryTest, ClaimIsExclusive)
{
	SessionRegistry registry(m_directory);
	std::string error;
	ASSERT_TRUE(registry.TryClaim("build-1", error)) << error;
	EXPECT_FALSE(registry.TryClaim("build-1", error));

	registry.ReleaseClaim("build-1");
	EXPECT_TRUE(registry.TryClaim("build-1", error)) << error;
}

TEST_F(SessionRegistryTest, DeadClaimIsRecovered)
{
	SessionRegistry registry(m_directory);
	std::ofstream(m_directory / "build-1.claim") << 0;

	std::string error;
	EXPECT_TRUE(registry.TryClaim("build-1", error)) << error;
}

TEST_F(SessionRegistryTest, AllocationMatchesGrammar)
{
	SessionRegistry registry(m_directory);
	const auto name = AllocateSessionName("C:/work/MyProject.neurus.json", registry);
	EXPECT_TRUE(IsValidSessionName(name));
	EXPECT_EQ(name.rfind("MyProject-", 0), 0u);
}

TEST_F(SessionRegistryTest, AllocationAvoidsExistingNames)
{
	SessionRegistry registry(m_directory);
	std::string error;
	const auto name = AllocateSessionName("", registry);
	ASSERT_TRUE(IsValidSessionName(name));
	ASSERT_TRUE(registry.Write(MakeRecord(name, CurrentPid()), error)) << error;

	const auto second = AllocateSessionName("", registry);
	EXPECT_TRUE(IsValidSessionName(second));
	EXPECT_NE(second, name);
}
