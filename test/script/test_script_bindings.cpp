/**
 * @file test_script_bindings.cpp
 * @brief The embedded `neurus` module exposes the scene for reading.
 *
 * A script run against a context whose scene is filled must see every
 * master-pool object through `neurus.data.objects` (iterable and keyed by UID)
 * and read live transform values. A run with no scene - or with no context at
 * all - must yield an empty collection without dereferencing anything.
 */

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <string>

#include "core/Log.h"
#include "core/RenderConfig.h"
#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "script/ScriptContext.h"
#include "script/ScriptEngine.h"

namespace
{

/**
 * @brief Runs @p code against @p ctx and reports whether it produced no error.
 *
 * `RunString` logs a raised (including failed `assert`) rather than throwing,
 * so an unchanged error count is how a test proves the snippet succeeded. The
 * context is generated per run and borrowed only for the call.
 */
bool RunCleanly(neurus::ScriptEngine& engine, const std::string& code,
                const neurus::ScriptContext& ctx = {})
{
	const std::size_t before = neurus::LogBuffer::instance().ErrorCount();
	engine.RunString(code, &ctx);
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
	ctx.scene  = &scene;
	ctx.config = &cfg;

	// Look the mesh up by UID: iteration order of the unordered master pool is
	// unspecified, so `objs[0]` could be either object.
	const std::string meshUid = std::to_string(mesh->GetObjectID());
	const std::string meshType = std::to_string(static_cast<int>(neurus::ObjectID::GOType::GO_MESH));

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

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

	EXPECT_TRUE(RunCleanly(engine, script, ctx));

	engine.Shutdown();
}

TEST(ScriptBindings, NoSceneYieldsNoObjects)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	// No context at all: the bindings must see an empty collection, not crash.
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
	ctx.scene  = &scene;
	ctx.config = &cfg;

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"assert list(neurus.data.objects) == []\n", ctx));

	engine.Shutdown();
}

TEST(ScriptBindings, ReadsTypedPropertiesAndConfig)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	// Exactly representable so the float-to-double equality below is sound.
	cfg.r_gamma = 2.0f;

	auto light = std::make_shared<neurus::Light>(neurus::LightType::POINTLIGHT, 3.0f,
	                                             glm::vec3(1.0f, 0.0f, 0.0f));
	auto camera = std::make_shared<neurus::Camera>();
	camera->cam_pers = 45.0f;
	camera->cam_near = 0.5f;
	camera->cam_far  = 250.0f;
	auto env = std::make_shared<neurus::Environment>();
	env->SetIntensity(2.5f);
	env->SetRotation(30.0f);

	scene.UseLight(light);
	scene.UseCamera(camera);
	scene.UseEnvironment(env);
	scene.ActivateCamera(camera->GetObjectID());

	neurus::ScriptContext ctx;
	ctx.scene  = &scene;
	ctx.config = &cfg;

	const std::string lightUid  = std::to_string(light->GetObjectID());
	const std::string camUid    = std::to_string(camera->GetObjectID());
	const std::string pointType = std::to_string(static_cast<int>(neurus::LightType::POINTLIGHT));

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	const std::string script =
		"import neurus\n"
		"light = neurus.data.lights[" + lightUid + "]\n"
		"assert light is not None\n"
		"assert light.power == 3.0\n"
		"assert light.type == " + pointType + "\n"
		"assert tuple(light.color) == (1.0, 0.0, 0.0)\n"
		"cam = neurus.context.active_camera\n"
		"assert cam is not None\n"
		"assert cam.uid == " + camUid + "\n"
		"assert cam.fov == 45.0\n"
		"assert cam.is_active\n"
		"env = list(neurus.data.environments)[0]\n"
		"assert env.intensity == 2.5\n"
		"assert env.rotation == 30.0\n"
		"assert neurus.data.config.gamma == 2.0\n";

	EXPECT_TRUE(RunCleanly(engine, script, ctx));
	engine.Shutdown();
}

TEST(ScriptBindings, EmptyContextHasNoActiveCamera)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"assert list(neurus.data.objects) == []\n"
		"assert neurus.context.active_camera is None\n"
		"assert neurus.data.config.gamma is None\n"));

	engine.Shutdown();
}

TEST(ScriptBindings, SetterRaisesReadOnlyErrorAndDoesNotMutate)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	auto mesh = std::make_shared<neurus::Mesh>();
	mesh->SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	scene.UseMesh(mesh);

	neurus::ScriptContext ctx;
	ctx.scene  = &scene;
	ctx.config = &cfg;

	const std::string meshUid = std::to_string(mesh->GetObjectID());

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	// The script catches ReadOnlyError, so the run logs no error (RunCleanly).
	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"o = neurus.data.objects[" + meshUid + "]\n"
		"raised = False\n"
		"try:\n"
		"    o.transform.position = (9.0, 9.0, 9.0)\n"
		"except neurus.ReadOnlyError:\n"
		"    raised = True\n"
		"assert raised\n"
		"try:\n"
		"    neurus.data.config.gamma = 3.0\n"
		"except neurus.ReadOnlyError:\n"
		"    pass\n"
		"else:\n"
		"    assert False\n", ctx));

	// The gated setter never wrote the live property.
	const glm::vec3 after = mesh->GetPosition();
	EXPECT_FLOAT_EQ(after.x, 1.0f);
	EXPECT_FLOAT_EQ(after.y, 2.0f);
	EXPECT_FLOAT_EQ(after.z, 3.0f);
	EXPECT_FLOAT_EQ(cfg.r_gamma, 1.0f);

	engine.Shutdown();
}
