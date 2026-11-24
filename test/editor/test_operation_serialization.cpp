/**
 * @file test_operation_serialization.cpp
 * @brief Operation-stack (undo/redo) serialization round-trip tests (no GPU).
 *
 * Two layers of coverage:
 *  1. Per-value-shape single-op round-trips through cereal polymorphic
 *     serialization: serialize a std::unique_ptr<Operation> to JSON and load it
 *     back, letting cereal reconstruct the concrete subclass from its registered
 *     type name (see operations/registrations/OperationRegistration.cpp). Replaying the restored op (and
 *     its inverse) proves uid + before + after all survived for each Value shape
 *     (float, bool, glm::vec3, VisibilityState, CameraPose, SelectionState).
 *  2. A full HistoryComponent save/load cycle: submit real edits, undo some so
 *     both stacks are non-empty, save, load into a fresh manager, and confirm
 *     the undo/redo stacks (and their replay behavior) match.
 */

#include <gtest/gtest.h>

#include <memory>
#include <sstream>

#include <cereal/archives/json.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/polymorphic.hpp>

#include "glm/glm.hpp"

#include "editor/controllers/ObjectController.hpp"
#include "editor/controllers/SceneController.hpp"
#include "editor/events/EventBus.hpp"
#include "editor/events/SceneEvents.hpp"
#include "editor/Input.hpp"
#include "editor/project/HistoryComponent.hpp"
#include "asset/ResourceManager.hpp"
#include "core/RenderConfig.hpp"
#include "asset/data/MeshData.hpp"
#include "shared/OperationTestAccess.hpp"
#include "editor/operations/OperationManager.hpp"
#include "editor/operations/registrations/OperationRegistration.hpp"
#include "editor/operations/SceneOperations.hpp"
#include "editor/operations/ScriptOperations.hpp"
#include "editor/events/ShaderEvents.hpp"
#include "editor/operations/ShaderOperations.hpp"
#include "shader/RenderShader.hpp"
#include "scene/Camera.hpp"
#include "scene/Light.hpp"
#include "scene/Mesh.hpp"
#include "scene/Scene.hpp"

using namespace neurus;

namespace {

/** @brief Serializes an op through a base pointer and reconstructs it via cereal. */
std::unique_ptr<Operation> RoundTrip(const std::unique_ptr<Operation>& op)
{
	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		out(cereal::make_nvp("op", op));
	}
	std::unique_ptr<Operation> restored;
	cereal::JSONInputArchive in(ss);
	in(cereal::make_nvp("op", restored));
	return restored;
}

} // namespace

class OperationSerializationTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_controller.Init(m_ctx);
		m_objectController.Init(m_ctx);

		m_mesh   = std::make_shared<Mesh>();
		m_light  = std::make_shared<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
		m_camera = m_resources.Load<Camera>();
		m_scene.UseMesh(m_mesh);
		m_scene.UseLight(m_light);
		m_scene.UseCamera(m_camera);
		m_scene.ActivateCamera(m_camera->GetObjectID());

		// The ShaderLinkOp restore handlers resolve the mesh via the POOL (the
		// Editor pattern), so the fixture mesh must also be pooled. The camera is
		// pooled the same way, for the same reason: camera-property ops replay
		// through ctx.resources. The light joins them because ObjectController
		// resolves every property write - and therefore every op replay - through
		// the pool.
		m_resources.Register(m_mesh);
		m_resources.Register(m_light);
	}

	void Process() { m_eventBus.Process(); }

	EventQueue m_eventBus;
	Scene m_scene;
	OperationManager m_operations{ m_eventBus };
	ResourceManager m_resources;
	RenderConfig m_config;
	ControllerContext m_ctx{ m_eventBus, m_resources, m_operations,
	                         [this]() { return &m_scene; },
	                         [this]() { return &m_config; } };
	SceneController m_controller;
	ObjectController m_objectController;
	std::shared_ptr<Mesh> m_mesh;
	std::shared_ptr<Light> m_light;
	std::shared_ptr<Camera> m_camera;
};

// --- Per-value-shape single-op round-trips ---------------------------------

TEST_F(OperationSerializationTest, PropertyOperationsPreserveTheirConcreteTypeAndBothEndpoints)
{
	auto ctx = OperationTestAccess::Context(m_eventBus);
	auto check = [&](const char* property, auto operation, auto readValue, auto before, auto after)
	{
		SCOPED_TRACE(property);
		using Op = typename decltype(operation)::element_type;
		std::unique_ptr<Operation> original = std::move(operation);
		auto restored = RoundTrip(original);
		ASSERT_NE(dynamic_cast<Op*>(restored.get()), nullptr);
		EXPECT_EQ(restored->Label(), original->Label());
		restored->Apply(ctx);
		EXPECT_EQ(readValue(), after);
		restored->Inverse()->Apply(ctx);
		EXPECT_EQ(readValue(), before);
	};
	const int light = m_light->GetObjectID();
	const int mesh = m_mesh->GetObjectID();
	check("light power", std::make_unique<SetLightPowerOp>(light, 10.0f, 42.0f),
	      [&] { return m_light->light_power; }, 10.0f, 42.0f);
	check("light shadow", std::make_unique<SetLightShadowOp>(light, true, false),
	      [&] { return m_light->use_shadow; }, true, false);
	check("position", std::make_unique<SetPositionOp>(mesh, glm::vec3(0.0f), glm::vec3(1, 2, 3)),
	      [&] { return m_mesh->GetPosition(); }, glm::vec3(0.0f), glm::vec3(1, 2, 3));
	check("visibility", std::make_unique<SetVisibilityOp>(mesh, VisibilityState{true, true}, VisibilityState{false, false}),
	      [&] { return std::pair{m_mesh->is_viewport, m_mesh->is_rendered}; },
	      std::pair{true, true}, std::pair{false, false});
}

TEST_F(OperationSerializationTest, CameraPoseOp_RoundTrip)
{
	const int uid = m_camera->GetObjectID();
	const glm::vec3 pos(2.0f, 5.0f, 1.0f);
	const glm::vec3 tar(1.0f, 0.0f, 0.0f);
	std::unique_ptr<Operation> op = std::make_unique<CameraTransformOp>(
		uid, CameraPose{ glm::vec3(0.0f), glm::vec3(0.0f) }, CameraPose{ pos, tar });

	auto restored = RoundTrip(op);
	ASSERT_NE(restored, nullptr);
	EXPECT_NE(dynamic_cast<CameraTransformOp*>(restored.get()), nullptr);

	auto ctx = OperationTestAccess::Context(m_eventBus);
	restored->Apply(ctx);
	EXPECT_EQ(m_camera->GetPosition(), pos);
	EXPECT_EQ(m_camera->cam_tar, tar);
}

TEST_F(OperationSerializationTest, SelectionStateOp_RoundTrip)
{
	SelectionState before{ {}, 0 };
	SelectionState after{ { m_mesh->GetObjectID(), m_light->GetObjectID() },
	                      m_light->GetObjectID() };
	std::unique_ptr<Operation> op = std::make_unique<SetSelectionOp>(before, after);

	auto restored = RoundTrip(op);
	ASSERT_NE(restored, nullptr);
	EXPECT_NE(dynamic_cast<SetSelectionOp*>(restored.get()), nullptr);
	EXPECT_TRUE(restored->PreservesRedo());

	auto ctx = OperationTestAccess::Context(m_eventBus);
	restored->Apply(ctx);
	EXPECT_TRUE(m_scene.selections.IsSelected(m_mesh.get()));
	EXPECT_TRUE(m_scene.selections.IsSelected(m_light.get()));
	EXPECT_EQ(m_scene.selections.GetActiveObject(), m_light.get());
}

TEST_F(OperationSerializationTest, SceneObjectAddOp_RoundTrip)
{
	auto mesh = m_resources.Load<Mesh>(m_resources.Load<MeshData>());
	const int uid = mesh->GetObjectID();
	std::unique_ptr<Operation> op = std::make_unique<SceneObjectAddOp>(std::vector<int>{uid}, true);

	auto restored = RoundTrip(op);
	ASSERT_NE(restored, nullptr);
	EXPECT_NE(dynamic_cast<SceneObjectAddOp*>(restored.get()), nullptr);
	EXPECT_EQ(restored->Label(), "Add Object");

	auto ctx = OperationTestAccess::Context(m_eventBus);
	restored->Apply(ctx);                       // add survived
	EXPECT_EQ(m_scene.mesh_list.count(uid), 1u);

	restored->Inverse()->Apply(ctx);            // uid + flag survived: delete inverse
	EXPECT_EQ(m_scene.mesh_list.count(uid), 0u);
}

TEST_F(OperationSerializationTest, ShaderLinkOp_RoundTrip)
{
	// Editor-style restore handlers (mirror Editor::Initialize).
	m_eventBus.Subscribe<ShaderLinkRestored>([this](const ShaderLinkRestored& e) {
		auto mesh = m_resources.Get<Mesh>(e.objectUid);
		if (!mesh) return;
		mesh->SetObjShader(m_resources.Get<RenderShader>(e.shaderId));
	});
	m_eventBus.Subscribe<ShaderUnlinkRestored>([this](const ShaderUnlinkRestored& e) {
		auto mesh = m_resources.Get<Mesh>(e.objectUid);
		if (!mesh) return;
		mesh->SetObjShader(nullptr);
	});

	auto shader = m_resources.Load<RenderShader>("MeshShader_2", "v.vert.glsl", "f.frag.glsl");
	const int shaderId = shader->GetObjectID();
	std::unique_ptr<Operation> op =
		std::make_unique<ShaderLinkOp>(m_mesh->GetObjectID(), shaderId, true);

	auto restored = RoundTrip(op);
	ASSERT_NE(restored, nullptr);
	EXPECT_NE(dynamic_cast<ShaderLinkOp*>(restored.get()), nullptr);
	EXPECT_EQ(restored->Label(), "Create Shader");

	auto ctx = OperationTestAccess::Context(m_eventBus);
	restored->Apply(ctx);
	EXPECT_EQ(m_mesh->o_shader.id(), shaderId); // link + shaderId survived

	restored->Inverse()->Apply(ctx);
	EXPECT_FALSE(m_mesh->o_shader);
	EXPECT_EQ(m_mesh->o_shader.id(), 0);        // uid + flag survived
}

TEST_F(OperationSerializationTest, CompositeOp_RoundTrip)
{
	auto mesh = m_resources.Load<Mesh>(m_resources.Load<MeshData>());
	const int uid = mesh->GetObjectID();
	SelectionState before{ {}, 0 };
	SelectionState after{ { uid }, uid };

	std::vector<std::unique_ptr<Operation>> seq;
	seq.push_back(std::make_unique<SceneObjectAddOp>(std::vector<int>{uid}, true));
	seq.push_back(std::make_unique<SetSelectionOp>(before, after));
	std::unique_ptr<Operation> op = std::make_unique<CompositeOp>(std::move(seq));

	auto restored = RoundTrip(op);
	ASSERT_NE(restored, nullptr);
	EXPECT_NE(dynamic_cast<CompositeOp*>(restored.get()), nullptr);

	auto ctx = OperationTestAccess::Context(m_eventBus);
	restored->Apply(ctx);                       // forward order: add then select
	EXPECT_EQ(m_scene.mesh_list.count(uid), 1u);
	EXPECT_TRUE(m_scene.selections.IsSelected(mesh.get()));

	auto inv = restored->Inverse();
	inv->Apply(ctx);                            // reversed inverses: select-before, remove
	EXPECT_EQ(m_scene.mesh_list.count(uid), 0u);
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 0u);
}

// --- Full HistoryComponent save/load cycle ---------------------------------

TEST_F(OperationSerializationTest, HistoryComponent_SaveLoad_BothStacks)
{
	// Record three edits, then undo one so BOTH stacks are populated.
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 20.0f });
	Process();
	m_eventBus.Enqueue(PositionChanged{ m_mesh->GetObjectID(), 1.0f, 2.0f, 3.0f });
	Process();
	m_eventBus.Enqueue(CameraFovChanged{ m_camera->GetObjectID(), 35.0f });
	Process();
	m_operations.Undo(); // camera fov edit → redo stack

	const HistoryView before = m_operations.GetHistoryView();
	ASSERT_EQ(before.undo.size(), 2u);
	ASSERT_EQ(before.redo.size(), 1u);

	// Save through the Serializable adapter.
	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		project::HistoryComponent saveComp(m_operations);
		saveComp.Save(out);
	}

	// Load into a completely fresh manager bound to the SAME event queue.
	OperationManager fresh{ m_eventBus };
	{
		cereal::JSONInputArchive in(ss);
		project::HistoryComponent loadComp(fresh);
		loadComp.Load(in);
	}

	const HistoryView after = fresh.GetHistoryView();
	EXPECT_EQ(after.undo, before.undo); // same labels, same order
	EXPECT_EQ(after.redo, before.redo);
	EXPECT_TRUE(fresh.CanUndo());
	EXPECT_TRUE(fresh.CanRedo());

	// Restored redo still reapplies the undone camera-fov edit.
	fresh.Redo();
	EXPECT_FLOAT_EQ(m_camera->cam_pers, 35.0f);
}

/**
 * @test A script run is saved and reloaded as a ScriptRunOp carrying its name:
 *       a file run's "Run demo.py" and a console run's default "Run Script" both
 *       survive, and Undo() puts the same labels on the redo list.
 */
TEST_F(OperationSerializationTest, ScriptRunOp_SurvivesHistorySaveLoad)
{
	// One file run (custom label) and one console run (default label).
	m_operations.BeginScriptScope("Run demo.py");
	m_eventBus.Enqueue(PositionChanged{ m_mesh->GetObjectID(), 4.0f, 5.0f, 6.0f });
	Process();
	m_operations.EndScriptScope();

	m_operations.BeginScriptScope();
	m_eventBus.Enqueue(PositionChanged{ m_mesh->GetObjectID(), 7.0f, 8.0f, 9.0f });
	Process();
	m_operations.EndScriptScope();

	const HistoryView before = m_operations.GetHistoryView();
	ASSERT_EQ(before.undo.size(), 2u);
	EXPECT_EQ(before.undo[0], "Run demo.py");
	EXPECT_EQ(before.undo[1], "Run Script");

	// Save through the Serializable adapter.
	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		project::HistoryComponent saveComp(m_operations);
		saveComp.Save(out);
	}

	// Load into a completely fresh manager bound to the SAME event queue.
	OperationManager fresh{ m_eventBus };
	{
		cereal::JSONInputArchive in(ss);
		project::HistoryComponent loadComp(fresh);
		loadComp.Load(in);
	}

	const HistoryView after = fresh.GetHistoryView();
	ASSERT_EQ(after.undo.size(), 2u);
	EXPECT_EQ(after.undo[0], "Run demo.py");
	EXPECT_EQ(after.undo[1], "Run Script");

	// Both entries kept their concrete type and name, so Undo() re-labels redo.
	ASSERT_NE(dynamic_cast<ScriptRunOp*>(fresh.GetUndoStack().front().get()), nullptr);
	ASSERT_NE(dynamic_cast<ScriptRunOp*>(fresh.GetUndoStack().back().get()), nullptr);
	fresh.Undo();
	ASSERT_EQ(fresh.GetHistoryView().redo.size(), 1u);
	EXPECT_EQ(fresh.GetHistoryView().redo.front(), "Run Script");
	fresh.Undo();
	ASSERT_EQ(fresh.GetHistoryView().redo.size(), 2u);
	EXPECT_EQ(fresh.GetHistoryView().redo.front(), "Run demo.py");
}

TEST_F(OperationSerializationTest, HistoryComponent_Load_MissingNode_ClearsHistory)
{
	// A project file without an "m_history" node (older format) must not throw
	// and must leave the manager with empty stacks.
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 20.0f });
	Process();
	ASSERT_TRUE(m_operations.CanUndo());

	std::stringstream ss;
	{
		cereal::JSONOutputArchive out(ss);
		out(cereal::make_nvp("unrelated", 123));
	}
	cereal::JSONInputArchive in(ss);
	project::HistoryComponent comp(m_operations);
	comp.Load(in);

	EXPECT_FALSE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());
}
