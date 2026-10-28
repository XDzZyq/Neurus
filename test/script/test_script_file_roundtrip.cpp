/**
 * @file test_script_file_roundtrip.cpp
 * @brief A real script file's history entry survives a project save and reopen.
 *
 * The script (`res/scripts/test.py`) performs three writes on a mesh named
 * "Ground". Running it through the real editor and engine records one
 * ScriptRunOp labelled "Run test.py"; this test saves the project WITH the
 * HistoryComponent, reopens it, and proves the restored entry keeps its label,
 * its concrete type and its undo, which rewinds all three writes together.
 *
 * No GPU and no Vulkan: `Editor(nullptr, nullptr)` skips every upload path. The
 * script path is relative to the build tree's `res/` copy, which CTest pins as
 * the working directory (the POST_BUILD step copies the whole `res/` tree).
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <utility>

#include "glm/glm.hpp"

#include "asset/Project.h"
#include "asset/components/EditorComponent.h"
#include "asset/components/HistoryComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/SceneComponent.h"
#include "core/ResourceManager.h"
#include "editor/Editor.h"
#include "editor/operations/ScriptOperations.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "script/ScriptContext.h"
#include "script/ScriptEngine.h"

// Force-link the polymorphic registration TUs (static libs).
#include "asset/registrations/DataRegistration.h"
#include "editor/operations/registrations/OperationRegistration.h"
#include "scene/registrations/TypeRegistration.h"

using namespace neurus;

namespace
{

/// Same starter mesh Application passes to CreateDefaultScene().
constexpr const char* kObj = "res/obj/sphere.obj";

/// The checked-in script fixture: three writes on the mesh named "Ground".
constexpr const char* kScript = "res/scripts/test.py";

/// The History label a file run derives from its basename.
constexpr const char* kLabel = "Run test.py";

struct TempFile
{
	std::string path;
	explicit TempFile(std::string p) : path(std::move(p)) {}
	~TempFile() { std::remove(path.c_str()); }
};

/**
 * @brief Registers the live components for save/load, in archive read order.
 *
 * Pool first (the Scene's ID references resolve against it), history next, and
 * the editor's view state last: registration order is read order. The viewport
 * camera test omits the HistoryComponent, so a project saved there carries no
 * undo history; this fixture registers it, which is the point of the round-trip.
 */
void BuildProject(project::Project& proj, Editor& editor)
{
	proj.Register<project::ResourceComponent>(editor.GetResourceManager());
	proj.Register<project::SceneComponent>(editor.GetScene(), editor.GetResourceManager());
	proj.Register<project::HistoryComponent>(editor.GetOperations());
	proj.Register<project::EditorComponent>(editor.GetViewport());
}

void SaveProject(const std::string& path, Editor& editor)
{
	project::Project proj;
	BuildProject(proj, editor);
	proj.Save(path);
}

/// @brief The Application::OnProjectOpen sequence, minus the UI layout.
void LoadProject(const std::string& path, Editor& editor)
{
	editor.BeginLoad();
	project::Project proj;
	BuildProject(proj, editor);
	proj.Load(path);
	editor.FinishLoad();
}

} // namespace

/**
 * @test Run a real `.py` file, save the project with its history, reopen it, and
 *       prove the one script entry keeps its label, its ScriptRunOp type and its
 *       undo - a single Undo must rewind all three writes.
 */
TEST(ScriptFileRoundTripTest, HistoryLabelAndUndoSurviveProjectSaveAndLoad)
{
	Editor editor(nullptr, nullptr);
	// Subscribes the Editor's own handlers and every controller, ScriptController
	// and ObjectController included. Without it a script write reaches no handler,
	// so nothing mutates and no undo entry is recorded.
	editor.Initialize();

	ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	engine.SetEventQueue(&editor.GetEventQueue());

	// The starter document, exactly as Application builds it on File > New.
	editor.NewScene();
	editor.CreateDefaultScene(kObj);

	// CreateDefaultScene pools the mesh under its constructor name "Mesh"; the
	// script addresses it by the document name "Ground".
	auto mesh = editor.GetResourceManager().GetByName<Mesh>("Mesh");
	ASSERT_NE(mesh, nullptr);
	mesh->o_name = editor.GetResourceManager().SetName(mesh->GetObjectID(), "Ground");

	const glm::vec3 originalPosition = mesh->GetPosition();
	const glm::vec3 originalScale = mesh->GetScale();
	const glm::vec3 originalRotation = mesh->GetRotation();

	// Run the checked-in script through the engine and the editor's controllers.
	ScriptContext ctx;
	editor.UpdateScriptContext(ctx);
	engine.RunFile(kScript, &ctx);

	ASSERT_EQ(editor.GetHistory().undo.size(), 1u);
	EXPECT_EQ(editor.GetHistory().undo.back(), kLabel);
	EXPECT_EQ(mesh->GetPosition(), glm::vec3(2.0f, 0.0f, 0.0f));
	EXPECT_EQ(mesh->GetScale(), glm::vec3(2.0f, 1.0f, 1.0f));
	EXPECT_EQ(mesh->GetRotation(), glm::vec3(0.0f, 0.0f, 45.0f));
	ASSERT_EQ(editor.GetOperations().GetUndoStack().size(), 1u);

	// Save, then reopen the way File > Open does. BeginLoad() resets the
	// document (scene, pool, config, history) before the components deserialize.
	TempFile tmp("test_script_file_roundtrip.neurus.json");
	SaveProject(tmp.path, editor);
	LoadProject(tmp.path, editor);

	// The history entry came back as a ScriptRunOp with the same name.
	ASSERT_EQ(editor.GetHistory().undo.size(), 1u);
	EXPECT_EQ(editor.GetHistory().undo.back(), kLabel);
	ASSERT_NE(dynamic_cast<ScriptRunOp*>(editor.GetOperations().GetUndoStack().front().get()),
	          nullptr);

	// The load seeder rebuilt the name registry from o_name, so "Ground" resolves.
	ResourceManager& resources = editor.GetResourceManager();
	auto restored = resources.GetByName<Mesh>("Ground");
	ASSERT_NE(restored, nullptr);

	// One undo rewinds the whole run: all three writes, not just the last.
	editor.GetOperations().Undo();
	EXPECT_EQ(restored->GetPosition(), originalPosition);
	EXPECT_EQ(restored->GetScale(), originalScale);
	EXPECT_EQ(restored->GetRotation(), originalRotation);

	ASSERT_EQ(editor.GetHistory().redo.size(), 1u);
	EXPECT_EQ(editor.GetHistory().redo.front(), kLabel);
}
