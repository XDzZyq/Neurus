/**
 * @file test_script_controller.cpp
 * @brief ScriptController: one script run becomes one undo step (no GPU).
 *
 * The controller is only the bridge between the two run-boundary events and the
 * operation sink's script scope; the compaction itself lives in OperationManager
 * and is covered by test_operation_manager.cpp. These tests therefore drive the
 * REAL wiring end to end: a run boundary, property writes applied by
 * ObjectController through the resource pool, then Undo/Redo on the single
 * composite entry the run produced.
 */

#include <gtest/gtest.h>

#include <memory>

#include "glm/glm.hpp"

#include "editor/controllers/ObjectController.h"
#include "editor/controllers/ScriptController.h"
#include "editor/events/EventBus.h"
#include "editor/events/SceneEvents.h"
#include "editor/events/ScriptEvents.h"
#include "editor/operations/OperationManager.h"
#include "core/ResourceManager.h"
#include "core/RenderConfig.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

using namespace neurus;

class ScriptControllerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_scriptController.Init(m_ctx);
		m_objectController.Init(m_ctx);

		// Pooled AND a scene member: the write resolves the UID through the pool,
		// while scene membership is what a real script run operates on.
		m_mesh = m_resources.Load<Mesh>();
		m_scene.UseMesh(m_mesh);
	}

	EventQueue m_eventBus;
	Scene m_scene;
	OperationManager m_operations{ m_eventBus };
	ResourceManager m_resources;
	RenderConfig m_config;
	ControllerContext m_ctx{ m_eventBus, m_resources, m_operations,
	                         [this]() { return &m_scene; },
	                         [this]() { return &m_config; } };
	ScriptController m_scriptController;
	ObjectController m_objectController;
	std::shared_ptr<Mesh> m_mesh;
};

TEST_F(ScriptControllerTest, RunBoundaryEventsOpenAndCloseTheScope)
{
	m_eventBus.Emit(ScriptRunStarted{});
	m_eventBus.Emit(PositionChanged{ m_mesh->GetObjectID(), 1.0f, 2.0f, 3.0f });
	m_eventBus.Emit(PositionChanged{ m_mesh->GetObjectID(), 4.0f, 5.0f, 6.0f });
	m_eventBus.Emit(ScriptRunFinished{});

	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u);
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(4.0f, 5.0f, 6.0f));
}

TEST_F(ScriptControllerTest, UndoOfARunRestoresTheRunStartState)
{
	m_eventBus.Emit(ScriptRunStarted{});
	m_eventBus.Emit(PositionChanged{ m_mesh->GetObjectID(), 1.0f, 2.0f, 3.0f });
	m_eventBus.Emit(PositionChanged{ m_mesh->GetObjectID(), 4.0f, 5.0f, 6.0f });
	m_eventBus.Emit(ScriptRunFinished{});

	// One undo rewinds the WHOLE run, not just its last write.
	m_operations.Undo();
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(0.0f, 0.0f, 0.0f));
	EXPECT_FALSE(m_operations.CanUndo());
	ASSERT_TRUE(m_operations.CanRedo());

	m_operations.Redo();
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(4.0f, 5.0f, 6.0f));
	EXPECT_FALSE(m_operations.CanRedo());
}

TEST_F(ScriptControllerTest, EditsOutsideARunStaySeparateEntries)
{
	// No run boundary: each write is its own history entry, so the scope can be
	// proven to close rather than leaking open past a run.
	m_eventBus.Emit(PositionChanged{ m_mesh->GetObjectID(), 1.0f, 2.0f, 3.0f });
	m_eventBus.Emit(PositionChanged{ m_mesh->GetObjectID(), 4.0f, 5.0f, 6.0f });

	EXPECT_EQ(m_operations.GetUndoStack().size(), 2u);
}
