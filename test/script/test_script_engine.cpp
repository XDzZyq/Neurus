#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/Log.hpp"
#include "script/ScriptConsole.hpp"
#include "script/ScriptEngine.hpp"

TEST(ScriptEngine, EvalReportsErrorsAndLogsTheNextResult)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	const std::size_t before = neurus::LogBuffer::instance().ErrorCount();
	EXPECT_EQ(engine.EvalString("1 / 0"), "");
	EXPECT_GT(neurus::LogBuffer::instance().ErrorCount(), before);
	EXPECT_FALSE(engine.EvalStringChecked("1 / 0").has_value());
	EXPECT_EQ(engine.EvalStringChecked("6 * 7").value_or(""), "42");
	engine.Console().Clear();
	EXPECT_EQ(engine.EvalString("1 + 2"), "3");

	bool logged = false;
	for (std::size_t i = 0; i < engine.Console().Size(); ++i)
	{
		const neurus::ScriptLine line = engine.Console().At(i);
		if (line.kind == neurus::ScriptLineKind::Result && line.text.find('3') != std::string::npos)
			logged = true;
	}
	EXPECT_TRUE(logged);
	engine.Shutdown();
}

TEST(ScriptEngine, CheckedRunDistinguishesStderrFromARaise)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	engine.Console().Clear();

	// stderr output alone is not a failed run: a script may log to it.
	EXPECT_TRUE(engine.RunStringChecked(
		"import sys; print('out'); print('err', file=sys.stderr)"));
	EXPECT_FALSE(engine.RunStringChecked("raise RuntimeError('boom')"));

	bool sawOutput = false;
	bool sawError = false;
	for (std::size_t i = 0; i < engine.Console().Size(); ++i)
	{
		const neurus::ScriptLine line = engine.Console().At(i);
		sawOutput = sawOutput
			|| (line.kind == neurus::ScriptLineKind::Output && line.text.find("out") != std::string::npos);
		sawError = sawError
			|| (line.kind == neurus::ScriptLineKind::Error && line.text.find("err") != std::string::npos);
	}
	EXPECT_TRUE(sawOutput);
	EXPECT_TRUE(sawError);
	engine.Shutdown();
}

TEST(ScriptEngine, ReportsPythonVersion)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	EXPECT_TRUE(engine.IsReady());
	EXPECT_FALSE(engine.PythonVersion().empty());
	EXPECT_NO_THROW((void)engine.EnvironmentPath());
	engine.Shutdown();
	EXPECT_FALSE(engine.IsReady());
	EXPECT_TRUE(engine.PythonVersion().empty());
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

TEST(ScriptEngine, CapturesErrorTracebackInConsole)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	engine.Console().Clear();
	const auto before = neurus::LogBuffer::instance().ErrorCount();
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
	EXPECT_GT(neurus::LogBuffer::instance().ErrorCount(), before);
	EXPECT_EQ(engine.EvalString("2 + 2"), "4");
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

TEST(ScriptConsoleBuffer, TruncatesHugeLines)
{
	neurus::ScriptConsoleBuffer buffer;
	buffer.Append(neurus::ScriptLineKind::Output,
	              std::string(neurus::kScriptConsoleMaxLine + 100, 'x'));

	ASSERT_EQ(buffer.Size(), 1u);
	const neurus::ScriptLine line = buffer.At(0);
	EXPECT_LT(line.text.size(), neurus::kScriptConsoleMaxLine + 32);
	EXPECT_NE(line.text.find("[truncated]"), std::string::npos);
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
