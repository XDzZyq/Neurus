/**
 * @file test_session_command.cpp
 * @brief Session, project and editor-command routing tests.
 */

#include <gtest/gtest.h>

#include "app/session/SessionCommandExecutor.hpp"
#include "command/Commands.hpp"

using namespace neurus;
using namespace neurus::command;

namespace
{

class FakeSessionState final : public ISessionState
{
public:
	int Pid() const override { return 1234; }
	std::string SessionName() const override { return "test-session"; }
	std::string ProjectPath() const override { return projectPath; }
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

	bool SaveAs(const std::string& path, std::string& error) override
	{
		if (saveFails)
		{
			error = "disk full";
			return false;
		}
		saveAsPath = path;
		projectPath = path;
		dirty = false;
		return true;
	}

	bool NewProject(bool force, std::string& error) override
	{
		if (dirty && !force)
		{
			error = "dirty";
			return false;
		}
		newCalled = true;
		projectPath.clear();
		dirty = false;
		objectCount = 7;
		return true;
	}

	bool OpenProject(const std::string& path, bool force, std::string& error) override
	{
		if (dirty && !force)
		{
			error = "dirty";
			return false;
		}
		openPath = path;
		projectPath = path;
		dirty = false;
		objectCount = 11;
		return true;
	}

	int SceneObjectCount() const override { return objectCount; }
	std::int64_t UptimeSeconds() const override { return 42; }
	std::string AppVersion() const override { return "0.1.0"; }

	std::string projectPath = "C:/work/p.neurus.json";
	bool dirty = false;
	bool saveFails = false;
	bool saved = false;
	std::string saveAsPath;
	bool newCalled = false;
	std::string openPath;
	int objectCount = 3;
};

class FakeEditorCommands final : public IEditorCommands
{
public:
	CommandResult Execute(const CommandRequest& request) override
	{
		lastRequest = request;
		return CommandResult::Success({{"delegated", request.name}});
	}

	CommandRequest lastRequest;
};

class FakeScriptCommands final : public IScriptCommands
{
public:
	CommandResult Execute(const CommandRequest& request) override
	{
		lastRequest = request;
		return CommandResult::Success({{"delegated", request.name}});
	}

	CommandRequest lastRequest;
};

class FakeSnapshotCommands final : public ISnapshotCommands
{
public:
	CommandResult Capture(const std::string& capturePath, bool all) override
	{
		path = capturePath;
		allAttachments = all;
		return CommandResult::Success({{"captured", capturePath}, {"all", all}});
	}

	std::string path;
	bool        allAttachments = false;
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
	EXPECT_EQ(result.details["objects"], 3);
	EXPECT_EQ(result.details["app_version"], "0.1.0");
	EXPECT_EQ(result.details["uptime_s"], 42);
	const auto info = executor.Execute({kProjectInfo, {}});
	ASSERT_TRUE(info.ok);
	EXPECT_EQ(info.details["path"], "C:/work/p.neurus.json");
	EXPECT_EQ(info.details["dirty"], true);
	EXPECT_EQ(info.details["objects"], 3);
}

TEST(SessionCommand, DirtySessionStaysOpenUntilSaved)
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

	state.saveFails = true;
	const auto failedSave = executor.Execute({kSessionClose, {{"save", true}}});
	EXPECT_FALSE(failedSave.ok);
	EXPECT_EQ(failedSave.code, CommandErrorCode::SaveFailed);
	EXPECT_EQ(failedSave.message, "disk full");
	EXPECT_FALSE(closeCalled);
	EXPECT_FALSE(executor.CloseRequested());
	state.saveFails = false;
	const auto saved = executor.Execute({kSessionClose, {{"save", true}}});
	EXPECT_TRUE(saved.ok);
	EXPECT_TRUE(state.saved);
	EXPECT_TRUE(closeCalled);
	EXPECT_TRUE(executor.CloseRequested());
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

TEST(SessionCommand, InvalidArgumentTypesLeaveSessionStateUnchanged)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});
	const CommandRequest requests[] = {
		{kSessionClose, {{"force", "yes"}}}, {kProjectOpen, {{"path", 12}}}
	};
	for (const auto& request : requests)
	{
		SCOPED_TRACE(request.name);
		const auto result = executor.Execute(request);
		EXPECT_FALSE(result.ok);
		EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
		EXPECT_FALSE(executor.CloseRequested());
		EXPECT_TRUE(state.openPath.empty());
	}
}

TEST(SessionCommand, ReplacingADirtyProjectRequiresForce)
{
	FakeSessionState state;
	state.dirty = true;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kProjectNew, {}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::DirtyProject);
	EXPECT_FALSE(state.newCalled);

	const auto forced = executor.Execute({kProjectNew, {{"force", true}}});
	ASSERT_TRUE(forced.ok);
	EXPECT_TRUE(state.newCalled);
	EXPECT_EQ(forced.details["path"], "");
	EXPECT_EQ(forced.details["dirty"], false);
	EXPECT_EQ(forced.details["objects"], 7);
}

TEST(SessionCommand, OpeningAProjectRequiresACleanStateOrForce)
{
	FakeSessionState state;
	state.dirty = true;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kProjectOpen, {{"path", "C:/work/next.neurus.json"}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::DirtyProject);
	EXPECT_TRUE(state.openPath.empty());

	state.dirty = false;
	const auto opened = executor.Execute({kProjectOpen, {{"path", "C:/work/next.neurus.json"}}});
	ASSERT_TRUE(opened.ok);
	EXPECT_EQ(state.openPath, "C:/work/next.neurus.json");
	EXPECT_EQ(opened.details["path"], "C:/work/next.neurus.json");
	EXPECT_EQ(opened.details["objects"], 11);
}

TEST(SessionCommand, SavesToTheCurrentOrExplicitPath)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kProjectSave, {}});
	ASSERT_TRUE(result.ok);
	EXPECT_TRUE(state.saved);
	EXPECT_EQ(result.details["path"], "C:/work/p.neurus.json");

	const auto savedAs = executor.Execute({kProjectSave, {{"path", "C:/work/out.neurus.json"}}});
	ASSERT_TRUE(savedAs.ok);
	EXPECT_EQ(state.saveAsPath, "C:/work/out.neurus.json");
	EXPECT_EQ(savedAs.details["path"], "C:/work/out.neurus.json");
	EXPECT_EQ(savedAs.details["saved"], true);
}

TEST(SessionCommand, DelegatesEditorCommandsAndPreservesArguments)
{
	FakeSessionState state;
	FakeEditorCommands editor;
	SessionCommandExecutor executor(state, [] {}, &editor);

	const auto result = executor.Execute({kSceneList, {{"type", "mesh"}}});
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.details["delegated"], "scene.list");
	EXPECT_EQ(editor.lastRequest.name, "scene.list");
	EXPECT_EQ(editor.lastRequest.args["type"], "mesh");

	for (const char* name : {kRenderGet, kRenderSet, kHistoryGet, kHistoryUndo,
	                         kHistoryRedo})
	{
		const auto result = executor.Execute({name, {}});
		ASSERT_TRUE(result.ok) << name;
		EXPECT_EQ(editor.lastRequest.name, name);
	}
}

TEST(SessionCommand, ScriptCommandsDelegateToTheScriptExecutor)
{
	FakeSessionState state;
	FakeScriptCommands script;
	SessionCommandExecutor executor(state, [] {}, nullptr, &script);

	const auto result = executor.Execute({kScriptRun, {{"code", "print(1)"}}});
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.details["delegated"], "script.run");
	EXPECT_EQ(script.lastRequest.name, "script.run");
	EXPECT_EQ(script.lastRequest.args["code"], "print(1)");

	for (const char* name : {kScriptEval, kScriptInfo, kScriptConsole, kScriptClear})
	{
		const auto routed = executor.Execute({name, {}});
		ASSERT_TRUE(routed.ok) << name;
		EXPECT_EQ(script.lastRequest.name, name);
	}
}

TEST(SessionCommand, SnapshotCommandsDelegateToTheSnapshotExecutor)
{
	FakeSessionState state;
	FakeSnapshotCommands snapshot;
	SessionCommandExecutor executor(state, [] {}, nullptr, nullptr, &snapshot);
	const auto missingPath = executor.Execute({kSnapshotCapture, {}});
	EXPECT_FALSE(missingPath.ok);
	EXPECT_EQ(missingPath.code, CommandErrorCode::InvalidArgument);
	EXPECT_TRUE(snapshot.path.empty());

	const auto result = executor.Execute(
		{kSnapshotCapture, {{"path", "C:/shots/out.png"}, {"all", true}}});
	ASSERT_TRUE(result.ok) << result.message;
	EXPECT_EQ(result.details["captured"], "C:/shots/out.png");
	EXPECT_EQ(result.details["all"], true);
	EXPECT_EQ(snapshot.path, "C:/shots/out.png");
	EXPECT_TRUE(snapshot.allAttachments);
}

TEST(SessionCommand, UnavailableCommandsReturnInvalidArgument)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});
	const CommandRequest requests[] = {
		{kSceneList, {}}, {kScriptRun, {{"code", "print(1)"}}},
		{kSnapshotCapture, {{"path", "out.png"}}}, {"nonsense.command", {}}
	};
	for (const auto& request : requests)
	{
		SCOPED_TRACE(request.name);
		const auto result = executor.Execute(request);
		EXPECT_FALSE(result.ok);
		EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
	}
}
