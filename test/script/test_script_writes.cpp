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
 * the pool, a config write, and coexistence with a queued UI event.
 */

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <string>

#include "core/Log.h"
#include "core/RenderConfig.h"
#include "core/ResourceManager.h"
#include "editor/controllers/ObjectController.h"
#include "editor/controllers/RenderConfigController.h"
#include "editor/controllers/SceneController.h"
#include "editor/controllers/ScriptController.h"
#include "editor/events/EventBus.h"
#include "editor/events/SceneEvents.h"
#include "editor/operations/OperationManager.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "script/ScriptContext.h"
#include "script/ScriptEngine.h"

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
		m_mesh->o_name = m_resources.SetName(m_mesh->GetObjectID(), "Ground");
		m_scene.UseMesh(m_mesh);

		m_detached = m_resources.Load<neurus::Mesh>();
		m_detached->o_name = m_resources.SetName(m_detached->GetObjectID(), "Detached");

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
	std::shared_ptr<neurus::Mesh>  m_mesh;
	std::shared_ptr<neurus::Mesh>  m_detached;
	neurus::ScriptEngine           m_engine;
};

}  // namespace

TEST_F(ScriptWrites, WriteIsVisibleOnTheNextStatement)
{
	RunCleanly(R"(
import neurus
mesh = neurus.data.meshes["Ground"]
mesh.position = (2, 2, 2)
assert tuple(mesh.position) == (2.0, 2.0, 2.0), mesh.position
)");

	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(2.0f, 2.0f, 2.0f));
}

TEST_F(ScriptWrites, RunIsOneUndoStep)
{
	RunCleanly(R"(
import neurus
mesh = neurus.data.meshes["Ground"]
mesh.position = (1, 0, 0)
mesh.position = (2, 0, 0)
mesh.position = (3, 0, 0)
)");

	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u);
	m_operations.Undo();
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(0.0f));
}

TEST_F(ScriptWrites, FailedRunKeepsOneUndoStep)
{
	Run("import neurus\nmesh = neurus.data.meshes['Ground']\n"
	    "mesh.position = (1, 0, 0)\n"
	    "raise RuntimeError('boom')\n");

	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(1.0f, 0.0f, 0.0f));
	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u);
}

TEST_F(ScriptWrites, DetachedObjectIsWritable)
{
	RunCleanly("import neurus\nneurus.data.meshes['Detached'].position = (5, 5, 5)\n");
	EXPECT_EQ(m_detached->GetPosition(), glm::vec3(5.0f));
}

TEST_F(ScriptWrites, WriteToDeletedObjectRaises)
{
	const int uid = m_detached->GetObjectID();
	RunCleanly("import neurus\nmesh = neurus.data.meshes['Detached']\n");
	m_resources.Remove(uid);

	Run("mesh.position = (1, 1, 1)\n");  // `mesh` persists in __main__
	EXPECT_GE(neurus::LogBuffer::instance().ErrorCount(), 1u);
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
	m_eventBus.Enqueue(neurus::PositionChanged{ m_mesh->GetObjectID(), 9.0f, 9.0f, 9.0f });
	RunCleanly("import neurus\nneurus.data.meshes['Ground'].position = (1, 1, 1)\n");

	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(1.0f));
	m_eventBus.Process();
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(9.0f));
}
