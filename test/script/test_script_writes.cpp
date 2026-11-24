/**
 * @file test_script_writes.cpp
 * @brief Scripted property writes are live: a setter mutates the document
 *        before the next Python statement runs.
 *
 * The fixture wires the REAL pipeline - an `EventQueue`, the script / object /
 * scene / render-config controllers, an `OperationManager` and a
 * `ResourceManager` - and attaches that queue to the engine, so each setter
 * emits its typed event synchronously into the controller that owns the write.
 * Only the engine and the event bus are stubbed out of the Application: there is
 * no renderer and no UI.
 *
 * Covered: read-after-write inside one run, one undo step per run (including a
 * raising run), pool-only (detached) objects, a write to an object that has left
 * the pool, a camera pose write, a config write, and coexistence with a queued UI
 * event.
 */

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <string>

#include "core/Log.hpp"
#include "core/RenderConfig.hpp"
#include "asset/ResourceManager.hpp"
#include "editor/Editor.hpp"
#include "editor/controllers/ObjectController.hpp"
#include "editor/controllers/RenderConfigController.hpp"
#include "editor/controllers/SceneController.hpp"
#include "editor/controllers/ScriptController.hpp"
#include "editor/events/EventBus.hpp"
#include "editor/events/SceneEvents.hpp"
#include "editor/operations/OperationManager.hpp"
#include "scene/Camera.hpp"
#include "scene/Mesh.hpp"
#include "scene/Scene.hpp"
#include "script/ScriptContext.hpp"
#include "script/ScriptEngine.hpp"

namespace
{

/**
 * @brief A document, the controllers that own its writes, and a live engine.
 *
 * Member order is load-bearing: the event bus and the operation manager come
 * before the context that references them, and the engine is declared last so it
 * is torn down before the document it hands to Python.
 */
class ScriptWrites : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_scriptController.Init(m_ctx);
		m_objectController.Init(m_ctx);
		m_sceneController.Init(m_ctx);
		m_configController.Init(m_ctx);

		// One mesh in the scene and one pooled-but-detached, as undo history
		// would hold it. Both are named through the pool's registry, which is
		// what `neurus.data.meshes[...]` resolves against.
		m_mesh = m_resources.Load<neurus::Mesh>();
		m_mesh->o_name = m_resources.SetName(m_mesh->RequireObjectID(), "Ground");
		m_scene.UseMesh(m_mesh);

		m_detached = m_resources.Load<neurus::Mesh>();
		m_detached->o_name = m_resources.SetName(m_detached->RequireObjectID(), "Detached");

		// Pooled and scene-detached, like the meshes above: `Load` names the
		// first camera "Camera", which is how the script addresses it.
		m_camera = m_resources.Load<neurus::Camera>();

		m_scriptCtx.scene     = &m_scene;
		m_scriptCtx.config    = &m_config;
		m_scriptCtx.resources = &m_resources;

		ASSERT_TRUE(m_engine.Initialize());
		m_engine.SetEventQueue(&m_eventBus);
	}

	void TearDown() override
	{
		m_engine.Shutdown();
	}

	/**
	 * @brief Runs @p code and asserts it raised nothing.
	 *
	 * `RunString` logs a raised (including a failed `assert`) rather than
	 * throwing, so an unchanged error count is how a test proves the snippet
	 * succeeded.
	 */
	void RunCleanly(const std::string& code)
	{
		const std::size_t before = neurus::LogBuffer::instance().ErrorCount();
		m_engine.RunString(code, &m_scriptCtx);
		EXPECT_EQ(neurus::LogBuffer::instance().ErrorCount(), before) << "script raised an error";
	}

	/// @brief Runs @p code, tolerating the error it is expected to raise.
	void Run(const std::string& code)
	{
		m_engine.RunString(code, &m_scriptCtx);
	}

	neurus::EventQueue        m_eventBus;
	neurus::Scene             m_scene;
	neurus::OperationManager  m_operations{ m_eventBus };
	neurus::ResourceManager   m_resources;
	neurus::RenderConfig      m_config;
	neurus::ControllerContext m_ctx{ m_eventBus, m_resources, m_operations,
	                                 [this]() { return &m_scene; },
	                                 [this]() { return &m_config; } };
	neurus::ScriptController       m_scriptController;
	neurus::ObjectController       m_objectController;
	neurus::SceneController        m_sceneController;
	neurus::RenderConfigController m_configController;
	neurus::ScriptContext          m_scriptCtx;
	std::shared_ptr<neurus::Mesh>   m_mesh;
	std::shared_ptr<neurus::Mesh>   m_detached;
	std::shared_ptr<neurus::Camera> m_camera;
	neurus::ScriptEngine            m_engine;
};

}  // namespace

TEST_F(ScriptWrites, RenameSetterIsUndoable)
{
	const std::size_t before = m_operations.GetUndoStack().size();
	RunCleanly(R"(
import neurus
mesh = neurus.data.meshes["Ground"]
mesh.name = "Ground Renamed"
assert mesh.name == "Ground Renamed"
)");
	EXPECT_EQ(m_mesh->o_name, "Ground Renamed");
	EXPECT_EQ(m_operations.GetUndoStack().size(), before + 1);

	m_operations.Undo();
	EXPECT_EQ(m_mesh->o_name, "Ground");
}

TEST_F(ScriptWrites, DeletingViaObjectOrSceneIsUndoable)
{
	const int uid = m_mesh->RequireObjectID();
	for (const char* operation : {"mesh.delete()", "neurus.scene.delete(mesh)"})
	{
		SCOPED_TRACE(operation);
		const auto before = m_operations.GetUndoStack().size();
		RunCleanly(std::string("import neurus\nmesh = neurus.data.meshes['Ground']\n") + operation);
		EXPECT_EQ(m_scene.GetObjectID(uid), nullptr);
		EXPECT_NE(m_resources.Get<neurus::Mesh>(uid), nullptr);
		ASSERT_EQ(m_operations.GetUndoStack().size(), before + 1);
		m_operations.Undo();
		ASSERT_EQ(m_scene.GetObjectID(uid), m_mesh.get());
	}
}

TEST_F(ScriptWrites, SceneSelectionIsWritableAndUndoable)
{
	const std::size_t before = m_operations.GetUndoStack().size();
	RunCleanly(R"(
import neurus
neurus.scene.select(neurus.data.meshes["Ground"])
assert [o.name for o in neurus.scene.selection()] == ["Ground"]
)");
	EXPECT_EQ(m_mesh->o_name, "Ground");
	EXPECT_EQ(m_operations.GetUndoStack().size(), before + 1);

	RunCleanly("neurus.scene.clear_selection()\nassert neurus.scene.selection() == []");
	EXPECT_TRUE(m_scene.selections.GetSelectedList().empty());
	m_operations.Undo();
	ASSERT_EQ(m_scene.selections.GetActiveObject(), m_mesh.get());

	m_operations.Undo();
	EXPECT_TRUE(m_scene.selections.GetSelectedList().empty());
}

TEST_F(ScriptWrites, VisibilityFlagsAreIndependentlyWritable)
{
	RunCleanly(R"(
import neurus
mesh = neurus.data.meshes["Ground"]
mesh.viewport_visible = False
mesh.render_visible = True
assert mesh.visible is False
assert mesh.viewport_visible is False
assert mesh.render_visible is True
)");
	EXPECT_FALSE(m_mesh->is_viewport);
	EXPECT_TRUE(m_mesh->is_rendered);
}

TEST(ScriptEditorWrites, ImportAddAndDuplicateCreateSceneContent)
{
	neurus::Editor editor(nullptr);
	editor.Initialize();
	editor.NewScene();

	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	engine.SetEventQueue(&editor.GetEventQueue());
	neurus::ScriptContext ctx;
	editor.UpdateScriptContext(ctx);

	ASSERT_TRUE(engine.RunStringChecked(R"(
import neurus
mesh = neurus.scene.import_mesh("res/obj/plane.obj", "Script Plane")
assert mesh.name == "Script Plane"
camera = neurus.scene.add_camera("Script Camera")
assert camera.name == "Script Camera"
copy = neurus.scene.duplicate(neurus.scene.meshes["Script Plane"], "Script Copy")
assert copy.name == "Script Copy"
assert len(neurus.scene.meshes) == 2
assert len(neurus.scene.cameras) == 1
)", &ctx));
	EXPECT_EQ(editor.GetScene().mesh_list.size(), 2u);
	EXPECT_EQ(editor.GetScene().cam_list.size(), 1u);

	// A missing file raises through Python; the host sees a failed run.
	EXPECT_FALSE(engine.RunStringChecked(
		"import neurus\nneurus.scene.import_mesh('missing-file.obj')\n", &ctx));
	EXPECT_EQ(editor.GetScene().mesh_list.size(), 2u);

	engine.Shutdown();
}

TEST_F(ScriptWrites, FailedRunKeepsOneUndoStep)
{
	Run("import neurus\nmesh = neurus.data.meshes['Ground']\n"
	    "mesh.position = (1, 0, 0)\n"
	    "raise RuntimeError('boom')\n");

	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(1.0f, 0.0f, 0.0f));
	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u);
}

/**
 * @test A scripted `Camera.position` write moves the camera AND records exactly
 *       one undoable Camera Transform, so the document and the undo stack agree.
 *       Before this, the setter emitted a pose event whose handler only replayed.
 */
TEST_F(ScriptWrites, CameraPositionIsOneUndoStep)
{
	const glm::vec3 before = m_camera->GetPosition();

	RunCleanly("import neurus\nneurus.data.cameras['Camera'].position = (5, 0, 0)\n");

	EXPECT_EQ(m_camera->GetPosition(), glm::vec3(5.0f, 0.0f, 0.0f));
	ASSERT_EQ(m_operations.GetUndoStack().size(), 1u);

	m_operations.Undo();
	EXPECT_EQ(m_camera->GetPosition(), before);
}

TEST_F(ScriptWrites, DetachedMeshRemainsWritableUntilRemovedFromThePool)
{
	const int uid = m_detached->RequireObjectID();
	RunCleanly("import neurus\nmesh = neurus.data.meshes['Detached']\nmesh.position = (5, 5, 5)\n");
	ASSERT_EQ(m_detached->GetPosition(), glm::vec3(5.0f));
	m_resources.Remove(uid);

	// `mesh` persists in __main__. The snippet asserts the failure itself, so a
	// write that silently succeeded (or raised the wrong type) fails the run.
	const std::size_t errors = neurus::LogBuffer::instance().ErrorCount();
	Run(R"(
try:
    mesh.position = (1, 1, 1)
except RuntimeError:
    pass
else:
    raise AssertionError("expected RuntimeError for a deleted object")
)");

	EXPECT_EQ(neurus::LogBuffer::instance().ErrorCount(), errors) << "the snippet's own assertion failed";
	EXPECT_EQ(m_detached->GetPosition(), glm::vec3(5.0f));
}

TEST_F(ScriptWrites, LoopSeesEachWrite)
{
	RunCleanly(R"(
import neurus
mesh = neurus.data.meshes["Ground"]
for i in range(1, 6):
    mesh.position = (i, 0, 0)
    assert tuple(mesh.position)[0] == float(i), mesh.position
)");
	ASSERT_EQ(m_operations.GetUndoStack().size(), 1u);
	m_operations.Undo();
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(0.0f));
}

TEST_F(ScriptWrites, TwoConfigWritesInOneRunBothApply)
{
	RunCleanly(R"(
import neurus
neurus.render.config.gamma = 2.0
assert neurus.render.config.gamma == 2.0, neurus.render.config.gamma
neurus.render.config.gamma = 3.0
)");

	EXPECT_FLOAT_EQ(m_config.r_gamma, 3.0f);
}

TEST_F(ScriptWrites, QueuedUiEventStillAppliesAfterTheRun)
{
	m_eventBus.Enqueue(neurus::PositionChanged{ m_mesh->RequireObjectID(), 9.0f, 9.0f, 9.0f });
	RunCleanly("import neurus\nneurus.data.meshes['Ground'].position = (1, 1, 1)\n");

	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(1.0f));
	m_eventBus.Process();
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(9.0f));
}
