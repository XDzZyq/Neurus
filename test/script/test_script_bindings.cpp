/**
 * @file test_script_bindings.cpp
 * @brief The embedded `neurus` module exposes the scene and the pool for
 *        reading.
 *
 * A script run sees every pooled object through `neurus.data.objects`
 * (iterable and keyed by UID) and the typed `neurus.data` collections, and reads
 * live values through proxies that resolve from the run's `resources` lookup.
 * Those collections are pool-wide, so a pooled object detached from the scene
 * stays visible; `neurus.scene.*` is scene-scoped and excludes it.
 * `neurus.scene.active_camera()` and `.selection()` are functions that read the
 * live scene, and `neurus.data.scene` is that same scene module object. A run
 * with no pool - or with no context at all - yields an empty collection without
 * dereferencing anything.
 */

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Log.h"
#include "core/RenderConfig.h"
#include "core/ResourceManager.h"
#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "script/ScriptContext.h"
#include "script/ScriptEngine.h"
#include "script/ScriptView.h"
#include "script/proxies/ObjectProxy.h"
#include "script/proxies/Transform3DProxy.h"

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
	neurus::ResourceManager resources;

	auto mesh = resources.Load<neurus::Mesh>();
	auto camera = resources.Load<neurus::Camera>();

	// Distinctive, non-symmetric TRS values: an axis swap, a transposed scale
	// or a wrong rotation component changes the exact tuples asserted below.
	mesh->SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	mesh->SetRotation(glm::vec3(10.0f, 20.0f, 30.0f));
	mesh->SetScale(glm::vec3(4.0f, 5.0f, 6.0f));

	scene.UseMesh(mesh);
	scene.UseCamera(camera);

	neurus::ScriptContext ctx;
	ctx.scene     = &scene;
	ctx.config    = &cfg;
	ctx.resources = &resources;

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

TEST(ScriptBindings, ProxyAndViewSurviveAcrossRuns)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	neurus::ResourceManager resources;

	auto mesh = resources.Load<neurus::Mesh>();
	// The registry name is authoritative; mirror the unique result into the
	// display name so the scene index and Python agree.
	mesh->o_name = resources.SetName(mesh->GetObjectID(), "Probe");
	scene.UseMesh(mesh);

	neurus::ScriptContext ctx;
	ctx.scene     = &scene;
	ctx.config    = &cfg;
	ctx.resources = &resources;

	const std::string meshUid = std::to_string(mesh->GetObjectID());

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	// `o` and `objects` land in `__main__`, which persists across runs. The
	// proxy stores only its UID and the view stores no run state, so both must
	// resolve against the document published by the second run.
	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"objects = neurus.data.objects\n"
		"o = objects[" + meshUid + "]\n"
		"assert o.name == 'Probe'\n", ctx));

	EXPECT_TRUE(RunCleanly(engine,
		"assert o.name == 'Probe'\n"
		"assert o.uid == " + meshUid + "\n"
		"assert objects is neurus.data.objects\n"
		"assert len(objects) == 1\n"
		"assert objects[" + meshUid + "].name == 'Probe'\n", ctx));

	engine.Shutdown();
}

TEST(ScriptBindings, NameLookupIsTypedAndExposesTransform)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	neurus::ResourceManager resources;

	auto mesh = resources.Load<neurus::Mesh>();
	mesh->o_name = resources.SetName(mesh->GetObjectID(), "Ground");
	mesh->SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));

	// Two meshes keep their default name: the registry suffixes the second one.
	auto second = resources.Load<neurus::Mesh>();
	second->o_name = resources.SetName(second->GetObjectID(), "Mesh");
	auto third = resources.Load<neurus::Mesh>();
	third->o_name = resources.SetName(third->GetObjectID(), "Mesh");
	auto camera = resources.Load<neurus::Camera>();
	camera->o_name = resources.SetName(camera->GetObjectID(), "Cam");

	scene.UseMesh(mesh);
	scene.UseMesh(second);
	scene.UseMesh(third);
	scene.UseCamera(camera);

	neurus::ScriptContext ctx;
	ctx.scene     = &scene;
	ctx.config    = &cfg;
	ctx.resources = &resources;

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	// The Python UID has to be the C++ identity, not merely self-consistent
	// with another proxy taken from the same source.
	const std::string meshUid = std::to_string(mesh->GetObjectID());

	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"ground = neurus.data.meshes['Ground']\n"
		"assert ground is not None\n"
		"assert isinstance(ground, neurus.data.Transform3D)\n"
		"assert isinstance(ground, neurus.data.Object)\n"
		"assert ground.name == 'Ground'\n"
		"assert tuple(ground.position) == (1.0, 2.0, 3.0)\n"
		"assert tuple(ground.rotation) == (0.0, 0.0, 0.0)\n"
		"assert tuple(ground.scale) == (1.0, 1.0, 1.0)\n"
		// Registration made the duplicate default name unique.
		"assert neurus.data.meshes['Mesh'] is not None\n"
		"assert neurus.data.meshes['Mesh.001'] is not None\n"
		// Absent name, and a name owned by another object type.
		"assert neurus.data.meshes['Nope'] is None\n"
		"assert neurus.data.meshes['Cam'] is None\n"
		"assert neurus.data.meshes['Ground'].uid == " + meshUid + "\n"
		"assert 'Ground' in neurus.data.meshes\n"
		"assert 'Nope' not in neurus.data.meshes\n", ctx));

	engine.Shutdown();
}

TEST(ScriptBindings, ResourceCollectionsIncludeDetachedObjects)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	neurus::ResourceManager resources;

	// meshA is used by the scene; meshB exists only in the pool, as a mesh held
	// by undo history would. Both are named through the registry, mirrored into
	// the display name so the proxy's `.name` agrees.
	auto meshA = resources.Load<neurus::Mesh>();
	meshA->o_name = resources.SetName(meshA->GetObjectID(), "Attached");
	auto meshB = resources.Load<neurus::Mesh>();
	meshB->o_name = resources.SetName(meshB->GetObjectID(), "Detached");
	scene.UseMesh(meshA);

	neurus::ScriptContext ctx;
	ctx.scene     = &scene;
	ctx.config    = &cfg;
	ctx.resources = &resources;

	const std::string uidB = std::to_string(meshB->GetObjectID());

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	// The pool, not the scene, is the collection's source: the detached mesh is
	// visible to iteration, to a name lookup and to a UID lookup.
	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"assert len(neurus.data.meshes) == 2\n"
		"detached = neurus.data.meshes['Detached']\n"
		"assert detached is not None\n"
		"assert detached.name == 'Detached'\n"
		"assert neurus.data.meshes[" + uidB + "].uid == " + uidB + "\n"
		"assert " + uidB + " in neurus.data.meshes\n"
		"assert 12345 not in neurus.data.meshes\n"
		"assert len(neurus.data.objects) == 2\n"
		"assert neurus.data.objects[" + uidB + "].name == 'Detached'\n"
		"assert " + uidB + " in neurus.data.objects\n"
		// D2: `objects` is UID-only, so a generic name resolves to None and
		// `in` agrees with that lookup.
		"assert neurus.data.objects['Detached'] is None\n"
		"assert 'Detached' not in neurus.data.objects\n", ctx));

	engine.Shutdown();
}

TEST(ScriptBindings, SceneCollectionExcludesDetachedObjects)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	neurus::ResourceManager resources;

	// meshA is used by the scene; meshB exists only in the pool, as a mesh held
	// by undo history would. Both are named through the registry, mirrored into
	// the display name so the proxy's `.name` agrees.
	auto meshA = resources.Load<neurus::Mesh>();
	meshA->o_name = resources.SetName(meshA->GetObjectID(), "Ground");
	auto meshB = resources.Load<neurus::Mesh>();
	meshB->o_name = resources.SetName(meshB->GetObjectID(), "Detached");
	scene.UseMesh(meshA);

	neurus::ScriptContext ctx;
	ctx.scene     = &scene;
	ctx.config    = &cfg;
	ctx.resources = &resources;

	const std::string uidA = std::to_string(meshA->GetObjectID());
	const std::string uidB = std::to_string(meshB->GetObjectID());

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	// The scene, not the pool, is this collection's source: the detached mesh is
	// invisible to iteration, to a name lookup and to a UID lookup, while the
	// attached one stays reachable through both keys.
	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"assert len(neurus.scene.meshes) == 1\n"
		"assert neurus.scene.meshes['Detached'] is None\n"
		"assert neurus.scene.meshes['Ground'] is not None\n"
		"assert neurus.scene.meshes[" + uidA + "].name == 'Ground'\n"
		"assert neurus.scene.meshes[" + uidB + "] is None\n"
		"assert " + uidA + " in neurus.scene.meshes\n"
		"assert " + uidB + " not in neurus.scene.meshes\n"
		"assert 'Ground' in neurus.scene.meshes\n"
		"assert 'Detached' not in neurus.scene.meshes\n"
		// `neurus.data.scene` is the scene module itself, not a second proxy.
		"assert neurus.data.scene is neurus.scene\n"
		// The other scene pools exist and are empty in this scene.
		"assert len(neurus.scene.cameras) == 0\n"
		"assert len(neurus.scene.lights) == 0\n"
		"assert len(neurus.scene.environments) == 0\n"
		"assert len(neurus.scene.objects) == 1\n"
		"assert neurus.scene.objects[" + uidA + "].name == 'Ground'\n"
		"assert neurus.scene.objects[" + uidB + "] is None\n"
		"assert " + uidA + " in neurus.scene.objects\n"
		// D2: `objects` is UID-only, so a generic name resolves to None and
		// `in` agrees with that lookup.
		"assert neurus.scene.objects['Ground'] is None\n"
		"assert 'Ground' not in neurus.scene.objects\n", ctx));

	engine.Shutdown();
}

TEST(ScriptBindings, SceneCollectionsMapEachScenePool)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	neurus::ResourceManager resources;

	// One named object per typed scene pool: a collection wired to the wrong
	// pool member would report another pool's length or resolve its name to None.
	auto camera = resources.Load<neurus::Camera>();
	camera->o_name = resources.SetName(camera->GetObjectID(), "SceneCam");
	auto light = resources.Load<neurus::Light>(
		neurus::LightType::POINTLIGHT, 3.0f, glm::vec3(1.0f));
	light->o_name = resources.SetName(light->GetObjectID(), "SceneLight");
	auto env = resources.Load<neurus::Environment>();
	env->o_name = resources.SetName(env->GetObjectID(), "SceneEnv");

	scene.UseCamera(camera);
	scene.UseLight(light);
	scene.UseEnvironment(env);

	neurus::ScriptContext ctx;
	ctx.scene     = &scene;
	ctx.config    = &cfg;
	ctx.resources = &resources;

	const std::string camUid   = std::to_string(camera->GetObjectID());
	const std::string lightUid = std::to_string(light->GetObjectID());
	const std::string envUid   = std::to_string(env->GetObjectID());

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"cam = neurus.scene.cameras['SceneCam']\n"
		"assert cam is not None and cam.uid == " + camUid + "\n"
		"assert neurus.scene.cameras[" + camUid + "].name == 'SceneCam'\n"
		"assert len(neurus.scene.cameras) == 1\n"
		"assert neurus.scene.cameras[" + lightUid + "] is None\n"
		"light = neurus.scene.lights['SceneLight']\n"
		"assert light is not None and light.uid == " + lightUid + "\n"
		"assert neurus.scene.lights[" + lightUid + "].name == 'SceneLight'\n"
		"assert len(neurus.scene.lights) == 1\n"
		"assert neurus.scene.lights[" + camUid + "] is None\n"
		"env = neurus.scene.environments['SceneEnv']\n"
		"assert env is not None and env.uid == " + envUid + "\n"
		"assert neurus.scene.environments[" + envUid + "].name == 'SceneEnv'\n"
		"assert len(neurus.scene.environments) == 1\n"
		"assert neurus.scene.environments[" + camUid + "] is None\n"
		"assert len(neurus.scene.objects) == 3\n", ctx));

	engine.Shutdown();
}

TEST(ScriptBindings, SceneFunctionsReadLiveState)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	neurus::ResourceManager resources;

	auto mesh = resources.Load<neurus::Mesh>();
	auto camera = resources.Load<neurus::Camera>();
	scene.UseMesh(mesh);
	scene.UseCamera(camera);
	scene.selections.Select(mesh.get(), /*increment=*/false);

	neurus::ScriptContext ctx;
	ctx.scene     = &scene;
	ctx.config    = &cfg;
	ctx.resources = &resources;

	const std::string meshUid = std::to_string(mesh->GetObjectID());
	const std::string camUid  = std::to_string(camera->GetObjectID());

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	// Functions, not properties: each call reads the live scene, so no value can
	// be captured and go stale.
	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"assert neurus.scene.active_camera() is None\n"
		"selection = neurus.scene.selection()\n"
		"assert [o.uid for o in selection] == [" + meshUid + "]\n"
		"assert isinstance(selection[0], neurus.data.Object)\n", ctx));

	// An activation made between runs is visible to the next call.
	scene.ActivateCamera(camera->GetObjectID());
	EXPECT_TRUE(RunCleanly(engine,
		"assert neurus.scene.active_camera().uid == " + camUid + "\n"
		"assert neurus.scene.active_camera().is_active\n", ctx));

	engine.Shutdown();
}

TEST(ScriptBindings, ObjectsCollectionEnumeratesEveryPooledObjectId)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	neurus::ResourceManager resources;

	// A bare ObjectID is the trap for ForEach<ObjectID>: it owns the exact-type
	// index entry, so that walk would see this one object and drop the camera
	// and light. The collection must enumerate through ForEachUID instead.
	auto base = resources.Load<neurus::ObjectID>();
	auto camera = resources.Load<neurus::Camera>();
	auto light = resources.Load<neurus::Light>(
		neurus::LightType::POINTLIGHT, 3.0f, glm::vec3(1.0f));

	int pooled = 0;
	resources.ForEachUID([&](const std::shared_ptr<neurus::UID>& obj)
	{
		if (std::dynamic_pointer_cast<neurus::ObjectID>(obj))
			++pooled;
	});
	ASSERT_EQ(pooled, 3) << "precondition: three ObjectIDs are pooled";

	neurus::ScriptContext ctx;
	ctx.scene     = &scene;
	ctx.config    = &cfg;
	ctx.resources = &resources;

	const std::string baseUid = std::to_string(base->GetObjectID());

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"assert len(neurus.data.objects) == " + std::to_string(pooled) + "\n"
		"assert " + baseUid + " in neurus.data.objects\n"
		"objs = neurus.data.objects\n"
		"assert objs[" + baseUid + "].transform is None\n"
		"assert {o.uid for o in objs} == {" + std::to_string(camera->GetObjectID())
		    + ", " + std::to_string(light->GetObjectID()) + ", " + baseUid + "}\n", ctx));

	engine.Shutdown();
}

TEST(ScriptBindings, NoSceneYieldsNoObjects)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	// No context at all (so no scene and no resource lookup): the bindings must
	// see an empty collection, not crash.
	EXPECT_TRUE(RunCleanly(engine,
		"import neurus\n"
		"assert list(neurus.data.objects) == []\n"
		"assert len(neurus.data.objects) == 0\n"
		"assert neurus.data.objects[12345] is None\n"
		"assert len(neurus.scene.meshes) == 0\n"
		"assert neurus.scene.meshes['Ground'] is None\n"
		"assert neurus.scene.objects[12345] is None\n"));

	engine.Shutdown();
}

TEST(ScriptBindings, EmptySceneYieldsNoObjects)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;
	neurus::ResourceManager resources;
	neurus::ScriptContext ctx;
	ctx.scene     = &scene;
	ctx.config    = &cfg;
	ctx.resources = &resources;

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
	neurus::ResourceManager resources;
	// Exactly representable so the float-to-double equality below is sound.
	cfg.r_gamma = 2.0f;

	auto light = resources.Load<neurus::Light>(
		neurus::LightType::POINTLIGHT, 3.0f, glm::vec3(1.0f, 0.0f, 0.0f));
	auto camera = resources.Load<neurus::Camera>();
	camera->cam_pers = 45.0f;
	camera->cam_near = 0.5f;
	camera->cam_far  = 250.0f;
	auto env = resources.Load<neurus::Environment>();
	env->SetIntensity(2.5f);
	env->SetRotation(30.0f);

	scene.UseLight(light);
	scene.UseCamera(camera);
	scene.UseEnvironment(env);
	scene.ActivateCamera(camera->GetObjectID());

	neurus::ScriptContext ctx;
	ctx.scene     = &scene;
	ctx.config    = &cfg;
	ctx.resources = &resources;

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
		"cam = neurus.scene.active_camera()\n"
		"assert cam is not None\n"
		"assert cam.uid == " + camUid + "\n"
		"assert cam.fov == 45.0\n"
		"assert cam.is_active\n"
		"assert isinstance(cam, neurus.data.Transform3D)\n"
		"env = list(neurus.data.environments)[0]\n"
		"assert env.intensity == 2.5\n"
		"assert env.rotation == 30.0\n"
		"assert isinstance(env, neurus.data.Transform3D)\n"
		"assert isinstance(light, neurus.data.Transform3D)\n"
		"assert isinstance(light, neurus.data.Object)\n"
		"assert tuple(light.position) == (0.0, 0.0, 0.0)\n"
		"assert neurus.render.config.gamma == 2.0\n";

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
		"assert neurus.scene.active_camera() is None\n"
		"assert neurus.render.config.gamma is None\n"));

	engine.Shutdown();
}

TEST(ScriptBindings, ProxiesResolveFromThePoolWithoutAScene)
{
	neurus::ResourceManager resources;
	auto mesh = resources.Load<neurus::Mesh>();

	// No scene at all: only the pool can answer a resolve.
	neurus::ScriptContext ctx;
	ctx.resources = &resources;
	neurus::script::ScriptView view{&ctx, nullptr};

	neurus::script::ObjectProxy object{mesh->GetObjectID(), &view};
	EXPECT_EQ(object.Resolve(), mesh.get());

	neurus::script::Transform3DProxy transform{mesh->GetObjectID(), &view};
	EXPECT_EQ(transform.Resolve(), static_cast<const neurus::Transform3D*>(mesh.get()));

	// A null lookup is a clean error, not a null dereference.
	neurus::ScriptContext empty;
	neurus::script::ScriptView emptyView{&empty, nullptr};
	neurus::script::ObjectProxy orphan{mesh->GetObjectID(), &emptyView};
	EXPECT_THROW(orphan.Resolve(), std::runtime_error);
}
