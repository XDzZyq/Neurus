/**
 * @file test_script_bindings.cpp
 * @brief The embedded `neurus` module exposes the scene for reading.
 *
 * A script run against a filled `ScriptContext` must see every master-pool
 * object through `neurus.data.objects` (iterable and keyed by UID) and read
 * live transform values. A run whose context has no scene - or whose scene
 * holds no objects - must yield an empty collection without dereferencing
 * anything.
 */

#include <gtest/gtest.h>

#include <glm/glm.hpp>

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

	// Distinctive, non-symmetric TRS values: an axis swap, a transposed scale
	// or a wrong rotation component changes the exact tuples asserted below.
	mesh->SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	mesh->SetRotation(glm::vec3(10.0f, 20.0f, 30.0f));
	mesh->SetScale(glm::vec3(4.0f, 5.0f, 6.0f));

	scene.UseMesh(mesh);
	scene.UseCamera(camera);

	neurus::ScriptContext ctx;
	neurus::BuildScriptContext(scene, cfg, ctx);

	// Look the mesh up by UID: iteration order of the unordered master pool is
	// unspecified, so `objs[0]` could be either object.
	const std::string meshUid = std::to_string(mesh->GetObjectID());
	const std::string meshType = std::to_string(static_cast<int>(neurus::ObjectID::GOType::GO_MESH));

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	engine.SetContext(ctx);

	const std::string script =
		"import neurus\n"
		"objs = list(neurus.data.objects)\n"
		"assert len(objs) == 2\n"
		"mesh = neurus.data.objects[" + meshUid + "]\n"
		"assert mesh is not None\n"
		// The integer-valued floats below are exactly representable, so exact
		// tuple equality is sound and catches an axis swap.
		"assert tuple(mesh.transform.position) == (1.0, 2.0, 3.0)\n"
		"assert tuple(mesh.transform.rotation) == (10.0, 20.0, 30.0)\n"
		"assert tuple(mesh.transform.scale) == (4.0, 5.0, 6.0)\n"
		"assert mesh.type == " + meshType + "\n"
		"assert isinstance(mesh.name, str)\n"
		"assert isinstance(mesh.visible, bool)\n";

	EXPECT_TRUE(RunCleanly(engine, script));

	engine.Shutdown();
}

TEST(ScriptBindings, NoSceneYieldsNoObjects)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	// No SetContext call: the run still publishes a context, but its borrowed
	// scene is null, which is the branch the bindings must survive.
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
