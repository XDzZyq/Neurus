/**
 * @file test_session_command.cpp
 * @brief Session command executor tests, including the close policy.
 */

#include <gtest/gtest.h>

#include "app/session/SessionCommandExecutor.h"
#include "command/Commands.h"

using namespace neurus;
using namespace neurus::command;

namespace
{

class FakeSessionState final : public ISessionState
{
public:
	int Pid() const override { return 1234; }
	std::string SessionName() const override { return "test-session"; }
	std::string ProjectPath() const override { return "C:/work/p.neurus.json"; }
	bool IsDirty() const override { return dirty; }

	bool Save(std::string& error) override
	{
		if (saveFails)
		{
			error = "disk full";
			return false;
		}
		saved = true;
		dirty = false;
		return true;
	}

	std::int64_t UptimeSeconds() const override { return 42; }
	std::string AppVersion() const override { return "0.1.0"; }

	bool dirty = false;
	bool saveFails = false;
	bool saved = false;
};

} // namespace

TEST(SessionCommand, StatusReportsDirtyAndPid)
{
	FakeSessionState state;
	state.dirty = true;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kSessionStatus, {}});
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.details["session"], "test-session");
	EXPECT_EQ(result.details["pid"], 1234);
	EXPECT_EQ(result.details["project"], "C:/work/p.neurus.json");
	EXPECT_EQ(result.details["dirty"], true);
	EXPECT_EQ(result.details["app_version"], "0.1.0");
	EXPECT_EQ(result.details["uptime_s"], 42);
}

TEST(SessionCommand, CloseRefusesDirtyWithoutSaveOrForce)
{
	FakeSessionState state;
	state.dirty = true;
	bool closeCalled = false;
	SessionCommandExecutor executor(state, [&] { closeCalled = true; });

	const auto result = executor.Execute({kSessionClose, {{"save", false}, {"force", false}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::DirtyProject);
	EXPECT_FALSE(closeCalled);
	EXPECT_FALSE(executor.CloseRequested());
}

TEST(SessionCommand, CloseSavePersistsThenRequestsClose)
{
	FakeSessionState state;
	state.dirty = true;
	bool closeCalled = false;
	SessionCommandExecutor executor(state, [&] { closeCalled = true; });

	const auto result = executor.Execute({kSessionClose, {{"save", true}}});
	EXPECT_TRUE(result.ok);
	EXPECT_TRUE(state.saved);
	EXPECT_TRUE(closeCalled);
	EXPECT_TRUE(executor.CloseRequested());
}

TEST(SessionCommand, SaveFailureIsReportedAndDoesNotClose)
{
	FakeSessionState state;
	state.dirty = true;
	state.saveFails = true;
	bool closeCalled = false;
	SessionCommandExecutor executor(state, [&] { closeCalled = true; });

	const auto result = executor.Execute({kSessionClose, {{"save", true}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::SaveFailed);
	EXPECT_EQ(result.message, "disk full");
	EXPECT_FALSE(closeCalled);
	EXPECT_FALSE(executor.CloseRequested());
}

TEST(SessionCommand, ForceClosesDirtyWithoutSaving)
{
	FakeSessionState state;
	state.dirty = true;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kSessionClose, {{"force", true}}});
	EXPECT_TRUE(result.ok);
	EXPECT_FALSE(state.saved);
	EXPECT_TRUE(executor.CloseRequested());
}

TEST(SessionCommand, NonBooleanArgumentIsRejected)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kSessionClose, {{"force", "yes"}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
	EXPECT_FALSE(executor.CloseRequested());
}

TEST(SessionCommand, UnknownCommandIsInvalidArgument)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({"scene.list", {}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}
