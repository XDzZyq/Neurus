/**
 * @file test_script_context.cpp
 * @brief `ScriptContext` reflects the live scene/config read view.
 *
 * `BuildScriptContext` is a Script-layer free function (not an `Editor` member,
 * see ruling R7): a member would force `editor -> script` and break the
 * unidirectional layer rule. These tests pin the two things the read path
 * promises a later run: the borrowed scene/config addresses and the snapshot of
 * the active camera plus selected UIDs.
 */

#include <gtest/gtest.h>

#include <vector>

#include "core/RenderConfig.h"
#include "core/ResourceManager.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "script/ScriptContext.h"
#include "script/ScriptEngine.h"

TEST(ScriptContextTest, ReflectsSceneSelectionAndActiveCamera)
{
	// Build a minimal pure-CPU scene: two cameras, so the activated one and the
	// selected one are distinguishable (activation and selection are separate).
	neurus::Scene scene;
	neurus::ResourceManager resources;
	neurus::RenderConfig cfg;

	auto viewed = resources.Load<neurus::Camera>();
	auto inspected = resources.Load<neurus::Camera>();
	scene.UseCamera(viewed);
	scene.UseCamera(inspected);
	ASSERT_TRUE(scene.ActivateCamera(viewed->GetObjectID()));
	scene.selections.Select(inspected.get(), /*increment=*/false);

	neurus::ScriptContext ctx;
	neurus::BuildScriptContext(scene, cfg, ctx);

	EXPECT_EQ(ctx.scene, &scene);
	EXPECT_EQ(ctx.config, &cfg);
	EXPECT_EQ(ctx.active_camera_id, scene.ActiveCameraID());
	EXPECT_EQ(ctx.active_camera_id, viewed->GetObjectID());
	ASSERT_EQ(ctx.selection.size(), 1u);
	EXPECT_EQ(ctx.selection.front(), inspected->GetObjectID());
}

TEST(ScriptContextTest, EmptySceneYieldsNoSelectionAndDefaultCamera)
{
	neurus::Scene scene;
	neurus::RenderConfig cfg;

	neurus::ScriptContext ctx;
	neurus::BuildScriptContext(scene, cfg, ctx);

	EXPECT_EQ(ctx.active_camera_id, scene.ActiveCameraID());
	EXPECT_EQ(ctx.active_camera_id, 0);
	EXPECT_TRUE(ctx.selection.empty());
}

TEST(ScriptContextTest, SetContextStoresACopyWithoutThrowing)
{
	// SetContext must be callable before Initialize (no Python required) and
	// must take a copy, so a temporary context is safe to pass.
	neurus::ScriptEngine engine;

	neurus::ScriptContext ctx;
	ctx.active_camera_id = 42;
	ctx.selection = {1, 2, 3};

	EXPECT_NO_THROW(engine.SetContext(ctx));
}
