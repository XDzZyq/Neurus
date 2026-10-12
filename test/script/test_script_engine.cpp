#include <gtest/gtest.h>

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
