#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/Log.h"
#include "core/RenderConfig.h"
#include "core/ResourceManager.h"
#include "editor/controllers/ObjectController.h"
#include "editor/controllers/ScriptController.h"
#include "editor/events/EventBus.h"
#include "editor/operations/OperationManager.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "script/ScriptConsole.h"
#include "script/ScriptEngine.h"

TEST(ScriptEngine, InitializesAndShutsDown)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	EXPECT_TRUE(engine.IsReady());
	engine.Shutdown();
	EXPECT_FALSE(engine.IsReady());
}

TEST(ScriptEngine, RunStringExecutesWithoutThrowing)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	EXPECT_NO_THROW(engine.RunString("x = 1 + 1\nassert x == 2"));
	engine.Shutdown();
}

TEST(ScriptEngine, RaisingScriptLogsErrorAndSurvives)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	const std::size_t before = neurus::LogBuffer::instance().ErrorCount();
	EXPECT_NO_THROW(engine.RunString("raise RuntimeError('boom')"));
	EXPECT_GT(neurus::LogBuffer::instance().ErrorCount(), before);
	EXPECT_NO_THROW(engine.RunString("y = 5"));  // engine still usable
	engine.Shutdown();
}

TEST(ScriptEngine, RunFileExecutesATempScript)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	const auto path = std::filesystem::temp_directory_path() / "neurus_runfile_test.py";
	std::ofstream(path) << "import builtins\n"
	                       "builtins.__neurus_ran = True\n"
	                       "print('neurus runfile output')\n";

	engine.Console().Clear();
	const std::size_t errorsBefore = neurus::LogBuffer::instance().ErrorCount();
	EXPECT_NO_THROW(engine.RunFile(path.string()));
	// A raising run would have logged an error instead of executing cleanly.
	EXPECT_EQ(neurus::LogBuffer::instance().ErrorCount(), errorsBefore);

	// Prove the file ran: the flag it set is visible to a later snippet
	// (a failed `assert` raises, which RunString logs rather than throws).
	const std::size_t beforeFlagCheck = neurus::LogBuffer::instance().ErrorCount();
	EXPECT_NO_THROW(engine.RunString("import builtins; assert builtins.__neurus_ran"));
	EXPECT_EQ(neurus::LogBuffer::instance().ErrorCount(), beforeFlagCheck);

	// Prove its stdout was captured into the console buffer.
	bool captured = false;
	for (std::size_t i = 0; i < engine.Console().Size(); ++i)
	{
		const neurus::ScriptLine line = engine.Console().At(i);
		if (line.kind == neurus::ScriptLineKind::Output
		    && line.text.find("neurus runfile output") != std::string::npos)
		{
			captured = true;
		}
	}
	EXPECT_TRUE(captured);

	engine.Shutdown();
	std::filesystem::remove(path);
}

TEST(ScriptEngine, FileRunRecordsOneEntryLabelledWithItsBasename)
{
	// The editor stack a file run writes through: the run boundary reaches
	// ScriptController -> OperationManager, and the property write reaches
	// ObjectController. Only the renderer and the UI are absent.
	neurus::EventQueue events;
	neurus::Scene scene;
	neurus::OperationManager operations{ events };
	neurus::ResourceManager resources;
	neurus::RenderConfig config;
	neurus::ControllerContext ctx{ events, resources, operations,
	                               [&scene]() { return &scene; },
	                               [&config]() { return &config; } };
	neurus::ScriptController scriptController;
	neurus::ObjectController objectController;
	scriptController.Init(ctx);
	objectController.Init(ctx);

	auto mesh = resources.Load<neurus::Mesh>();
	mesh->o_name = resources.SetName(mesh->GetObjectID(), "Ground");
	scene.UseMesh(mesh);

	neurus::ScriptContext scriptCtx;
	scriptCtx.scene = &scene;
	scriptCtx.config = &config;
	scriptCtx.resources = &resources;

	// Declared last so the engine is torn down before the document it hands to
	// Python, matching the other scripted-write fixture.
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	engine.SetEventQueue(&events);

	const auto path = std::filesystem::temp_directory_path() / "neurus_runfile_label_test.py";
	std::ofstream(path) << "import neurus\n"
	                       "neurus.data.meshes['Ground'].position = (1, 2, 3)\n";

	EXPECT_NO_THROW(engine.RunFile(path.string(), &scriptCtx));
	std::filesystem::remove(path);

	ASSERT_EQ(operations.GetHistoryView().undo.size(), 1u);
	EXPECT_EQ(operations.GetHistoryView().undo.back(), "Run neurus_runfile_label_test.py");

	engine.Shutdown();
}

TEST(ScriptEngine, CapturesStdoutInConsole)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	EXPECT_NO_THROW(engine.RunString("print('neurus console capture')"));

	bool found = false;
	for (std::size_t i = 0; i < engine.Console().Size(); ++i)
	{
		const neurus::ScriptLine line = engine.Console().At(i);
		if (line.kind == neurus::ScriptLineKind::Output
		    && line.text.find("neurus console capture") != std::string::npos)
		{
			found = true;
		}
	}
	EXPECT_TRUE(found);
	engine.Shutdown();
}

TEST(ScriptEngine, CapturesErrorTracebackInConsole)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	engine.Console().Clear();
	EXPECT_NO_THROW(engine.RunString("raise ValueError('neurus boom')"));

	bool found = false;
	for (std::size_t i = 0; i < engine.Console().Size(); ++i)
	{
		const neurus::ScriptLine line = engine.Console().At(i);
		if (line.kind == neurus::ScriptLineKind::Error
		    && line.text.find("neurus boom") != std::string::npos)
		{
			found = true;
		}
	}
	EXPECT_TRUE(found);
	engine.Shutdown();
}

TEST(ScriptEngine, RunsQueuedScriptsOnExecutePending)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	// Two snippets queued in order; the second builds on the first, so a run
	// out of order or a skipped entry fails the assertion after the drain.
	engine.Submit("import builtins\nbuiltins.__neurus_q = 1");
	engine.Submit("import builtins\nbuiltins.__neurus_q = builtins.__neurus_q + 41");

	engine.ExecutePending(neurus::ScriptContext{});

	const std::size_t before = neurus::LogBuffer::instance().ErrorCount();
	engine.RunString("import builtins\nassert builtins.__neurus_q == 42");
	EXPECT_EQ(neurus::LogBuffer::instance().ErrorCount(), before)
		<< "queued scripts did not run in order";

	engine.Shutdown();
}

TEST(ScriptConsoleBuffer, RingDropsOldestAndAssignsSeq)
{
	neurus::ScriptConsoleBuffer buffer;
	for (int i = 0; i < 5; ++i)
	{
		buffer.Append(neurus::ScriptLineKind::Input, std::to_string(i));
	}
	EXPECT_EQ(buffer.Size(), 5u);
	EXPECT_EQ(buffer.At(0).text, "0");
	EXPECT_LT(buffer.At(0).seq, buffer.At(4).seq);

	// Overflow: push past capacity and confirm the oldest lines are dropped.
	for (std::size_t i = 5; i < neurus::kScriptConsoleCapacity + 10; ++i)
	{
		buffer.Append(neurus::ScriptLineKind::Output, std::to_string(i));
	}
	EXPECT_EQ(buffer.Size(), neurus::kScriptConsoleCapacity);
	EXPECT_EQ(buffer.At(0).text, "10");  // 0..9 evicted
	EXPECT_EQ(buffer.At(neurus::kScriptConsoleCapacity - 1).text,
	          std::to_string(neurus::kScriptConsoleCapacity + 9));

	// Clear() empties the buffer and the ring restarts cleanly afterwards.
	buffer.Clear();
	EXPECT_EQ(buffer.Size(), 0u);

	for (int i = 0; i < 3; ++i)
	{
		buffer.Append(neurus::ScriptLineKind::Input, std::to_string(i));
	}
	EXPECT_EQ(buffer.Size(), 3u);
	buffer.Clear();
	EXPECT_EQ(buffer.Size(), 0u);

	// A post-Clear append is visible at logical index 0.
	buffer.Append(neurus::ScriptLineKind::Output, "after clear");
	EXPECT_EQ(buffer.Size(), 1u);
	EXPECT_EQ(buffer.At(0).text, "after clear");
}
