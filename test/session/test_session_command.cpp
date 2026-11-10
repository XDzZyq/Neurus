/**
 * @file test_session_command.cpp
 * @brief Session, project and editor-command routing tests.
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

TEST(SessionCommand, ProjectInfoReportsTheDocument)
{
	FakeSessionState state;
	state.dirty = true;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kProjectInfo, {}});
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.details["path"], "C:/work/p.neurus.json");
	EXPECT_EQ(result.details["dirty"], true);
	EXPECT_EQ(result.details["objects"], 3);
}

TEST(SessionCommand, ProjectNewRefusesDirtyWithoutForce)
{
	FakeSessionState state;
	state.dirty = true;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kProjectNew, {}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::DirtyProject);
	EXPECT_FALSE(state.newCalled);
}

TEST(SessionCommand, ProjectNewForceReplacesTheDocument)
{
	FakeSessionState state;
	state.dirty = true;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kProjectNew, {{"force", true}}});
	ASSERT_TRUE(result.ok);
	EXPECT_TRUE(state.newCalled);
	EXPECT_EQ(result.details["path"], "");
	EXPECT_EQ(result.details["dirty"], false);
	EXPECT_EQ(result.details["objects"], 7);
}

TEST(SessionCommand, ProjectOpenRequiresAStringPath)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	auto result = executor.Execute({kProjectOpen, {{"path", 12}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
	EXPECT_TRUE(state.openPath.empty());
}

TEST(SessionCommand, ProjectOpenLoadsThePath)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kProjectOpen, {{"path", "C:/work/next.neurus.json"}}});
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(state.openPath, "C:/work/next.neurus.json");
	EXPECT_EQ(result.details["path"], "C:/work/next.neurus.json");
	EXPECT_EQ(result.details["objects"], 11);
}

TEST(SessionCommand, ProjectOpenRefusesDirtyWithoutForce)
{
	FakeSessionState state;
	state.dirty = true;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kProjectOpen, {{"path", "C:/work/next.neurus.json"}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::DirtyProject);
	EXPECT_TRUE(state.openPath.empty());
}

TEST(SessionCommand, ProjectSaveToAPathUsesSaveAs)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kProjectSave, {{"path", "C:/work/out.neurus.json"}}});
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(state.saveAsPath, "C:/work/out.neurus.json");
	EXPECT_EQ(result.details["path"], "C:/work/out.neurus.json");
	EXPECT_EQ(result.details["saved"], true);
}

TEST(SessionCommand, ProjectSaveWithoutPathUsesTheCurrentPath)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kProjectSave, {}});
	ASSERT_TRUE(result.ok);
	EXPECT_TRUE(state.saved);
	EXPECT_EQ(result.details["path"], "C:/work/p.neurus.json");
}

TEST(SessionCommand, SceneAndObjectCommandsDelegateToTheEditor)
{
	FakeSessionState state;
	FakeEditorCommands editor;
	SessionCommandExecutor executor(state, [] {}, &editor);

	const auto result = executor.Execute({kSceneList, {{"type", "mesh"}}});
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.details["delegated"], "scene.list");
	EXPECT_EQ(editor.lastRequest.name, "scene.list");
	EXPECT_EQ(editor.lastRequest.args["type"], "mesh");
}

TEST(SessionCommand, RenderAndHistoryCommandsDelegateToTheEditor)
{
	FakeSessionState state;
	FakeEditorCommands editor;
	SessionCommandExecutor executor(state, [] {}, &editor);

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
}

TEST(SessionCommand, ScriptCommandsAreUnavailableWithoutAnExecutor)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kScriptRun, {{"code", "print(1)"}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}

TEST(SessionCommand, SnapshotCommandsDelegateToTheSnapshotExecutor)
{
	FakeSessionState state;
	FakeSnapshotCommands snapshot;
	SessionCommandExecutor executor(state, [] {}, nullptr, nullptr, &snapshot);

	const auto result = executor.Execute(
		{kSnapshotCapture, {{"path", "C:/shots/out.png"}, {"all", true}}});
	ASSERT_TRUE(result.ok) << result.message;
	EXPECT_EQ(result.details["captured"], "C:/shots/out.png");
	EXPECT_EQ(result.details["all"], true);
	EXPECT_EQ(snapshot.path, "C:/shots/out.png");
	EXPECT_TRUE(snapshot.allAttachments);
}

TEST(SessionCommand, SnapshotRequiresAPathAndAnExecutor)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	auto result = executor.Execute({kSnapshotCapture, {{"path", "C:/shots/out.png"}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);

	FakeSnapshotCommands snapshot;
	SessionCommandExecutor withSnapshot(state, [] {}, nullptr, nullptr, &snapshot);
	result = withSnapshot.Execute({kSnapshotCapture, {}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}

TEST(SessionCommand, SceneCommandsAreUnknownWithoutAnEditor)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({kSceneList, {}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}

TEST(SessionCommand, UnknownCommandIsInvalidArgument)
{
	FakeSessionState state;
	SessionCommandExecutor executor(state, [] {});

	const auto result = executor.Execute({"nonsense.command", {}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}
