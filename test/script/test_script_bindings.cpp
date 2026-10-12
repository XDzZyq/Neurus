/**
 * @file test_script_bindings.cpp
 * @brief The embedded `neurus` module exposes the scene for reading.
 *
 * A script run against a filled `ScriptContext` must see every master-pool
 * object through `neurus.data.objects` (iterable and keyed by UID) and read
 * live transform values. A run with no context - or with a scene that holds no
 * objects - must yield an empty collection without dereferencing anything.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>

#include "core/Log.h"
#include "core/RenderConfig.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "script/ScriptContext.h"
#include "script/ScriptEngine.h"

namespace
{

/**
 * @brief Runs @p code and reports whether it produced no Python error.
 *
 * `RunString` logs a raised (including failed `assert`) rather than throwing,
 * so an unchanged error count is how a test proves the snippet succeeded.
 */
bool RunCleanly(neurus::ScriptEngine& engine, const std::string& code)
{
	const std::size_t before = neurus::LogBuffer::instance().ErrorCount();
	engine.RunString(code);
	return neurus::LogBuffer::instance().ErrorCount() == before;
}

}  // namespace

TEST(ScriptBindings, ReadsSceneObjects)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;

	auto mesh = std::make_shared<neurus::Mesh>();
	auto camera = std::make_shared<neurus::Camera>();
	scene.UseMesh(mesh);
	scene.UseCamera(camera);

	neurus::ScriptContext ctx;
	neurus::BuildScriptContext(scene, cfg, ctx);

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	engine.SetContext(ctx);

	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"objs = list(neurus.data.objects)\n"
		"assert len(objs) >= 2\n"
		"p = objs[0].transform.position\n"
		"assert len(p) == 3\n"
		// Indexing by UID returns the same object and reading the remaining
		// properties does not raise.
		"assert neurus.data.objects[objs[0].uid].uid == objs[0].uid\n"
		"assert objs[0].name is not None\n"
		"assert isinstance(objs[0].type, int)\n"
		"assert isinstance(objs[0].visible, bool)\n"));

	engine.Shutdown();
}

TEST(ScriptBindings, EmptyContextYieldsNoObjects)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	// No SetContext call: the bindings must see a null context and stay safe.
	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"assert list(neurus.data.objects) == []\n"
		"assert len(neurus.data.objects) == 0\n"
		"assert neurus.data.objects[12345] is None\n"));

	engine.Shutdown();
}

TEST(ScriptBindings, EmptySceneYieldsNoObjects)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	neurus::ScriptContext ctx;
	neurus::BuildScriptContext(scene, cfg, ctx);

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	engine.SetContext(ctx);

	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"assert list(neurus.data.objects) == []\n"));

	engine.Shutdown();
}
