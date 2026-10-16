/**
 * @file test_script_context.cpp
 * @brief `ScriptContext` is a two-pointer read view; the scene is the source of
 *        truth for the active camera and the selection.
 *
 * The type carries only the borrowed scene/config pointers. It is generated
 * fresh per run (`Editor::UpdateScriptContext`) and discarded afterwards - there
 * is no persistent, process-wide context. The active camera and the selection
 * are read from the scene live, so nothing here can drift from the scene; the
 * shape test below is what pins that. The header is Qt-/Vulkan-free and
 * forward-declares both types, which is itself part of what is asserted (this
 * file needs no QApplication).
 */

#include <gtest/gtest.h>

#include <vector>

#include "core/RenderConfig.h"
#include "core/ResourceManager.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "script/ScriptContext.h"

using namespace neurus;

// The Editor's half is assignable - what Editor::UpdateScriptContext() relies on.
TEST(ScriptContextShape, IsTwoPlainPointers)
{
	Scene        scene{};
	RenderConfig config{};

	ScriptContext ctx{};
	ctx.scene  = &scene;
	ctx.config = &config;

	EXPECT_EQ(ctx.scene, &scene);
	EXPECT_EQ(ctx.config, &config);
}

TEST(ScriptContextShape, DefaultsToNoSceneOrConfig)
{
	const ScriptContext ctx{};
	EXPECT_EQ(ctx.scene, nullptr);
	EXPECT_EQ(ctx.config, nullptr);
}

// The scene, not the context, owns activation and selection.
TEST(ScriptContextShape, SceneOwnsActiveCameraAndSelection)
{
	Scene scene;
	ResourceManager resources;

	auto viewed    = resources.Load<Camera>();
	auto inspected = resources.Load<Camera>();
	scene.UseCamera(viewed);
	scene.UseCamera(inspected);
	ASSERT_TRUE(scene.ActivateCamera(viewed->GetObjectID()));
	scene.selections.Select(inspected.get(), /*increment=*/false);

	// The two facts the bindings read directly off the scene.
	EXPECT_EQ(scene.ActiveCameraID(), viewed->GetObjectID());

	std::vector<int> selected;
	int activeUid = 0;
	SnapshotSelectionUids(scene.selections, selected, activeUid);
	ASSERT_EQ(selected.size(), 1u);
	EXPECT_EQ(selected.front(), inspected->GetObjectID());
}
