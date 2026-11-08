/**
 * @file test_operation_manager.cpp
 * @brief Undo/redo history + operation replay tests (no GPU).
 *
 * These exercise the full Phase 1 slice through the real controller path:
 * a scene event mutates state AND records an operation, then Undo/Redo replay
 * synthesized inverse/forward events synchronously. Tests cover round-trip,
 * involution, recording suppression during replay, redo invalidation, and
 * stale-identity no-op.
 */

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

#include "glm/glm.hpp"

#include "editor/controllers/ObjectController.h"
#include "editor/controllers/SceneController.h"
#include "editor/events/EventBus.h"
#include "editor/events/SceneEvents.h"
#include "editor/Input.h"
#include "core/ResourceManager.h"
#include "editor/operations/OperationContext.h"
#include "editor/operations/OperationManager.h"
#include "editor/operations/SceneOperations.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "scene/ObjectID.h"
#include "core/RenderConfig.h"

using namespace neurus;

namespace {

/**
 * @brief UID used by the script-scope tests that assert buffering and commit only.
 *
 * Deliberately unpooled: those tests never claim replay semantics, which are
 * covered by ScriptScopeCompactsIntoOneEntry and test_editor/test_script_controller.cpp.
 */
constexpr int kScriptUid = 9001;

} // anonymous namespace

class OperationManagerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_controller.Init(m_ctx);
		m_objectController.Init(m_ctx);

		m_mesh   = std::make_shared<Mesh>();
		m_light  = std::make_shared<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
		// Pooled: property writes and their op replay resolve through
		// ctx.resources, so a non-pooled object would drop every edit.
		m_resources.Register(m_mesh);
		m_resources.Register(m_light);
		// Pooled: camera ops are replayed against ctx.resources, not cam_list.
		m_camera = m_resources.Load<Camera>();
		m_scene.UseMesh(m_mesh);
		m_scene.UseLight(m_light);
		m_scene.UseCamera(m_camera);
		m_scene.ActivateCamera(m_camera->GetObjectID());
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

// --- Round-trip ------------------------------------------------------------

TEST_F(OperationManagerTest, LightPower_RoundTrip)
{
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 42.0f });
	Process();
	EXPECT_FLOAT_EQ(m_light->light_power, 42.0f);
	ASSERT_TRUE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());

	m_operations.Undo();
	EXPECT_FLOAT_EQ(m_light->light_power, 10.0f);
	EXPECT_FALSE(m_operations.CanUndo());
	ASSERT_TRUE(m_operations.CanRedo());

	m_operations.Redo();
	EXPECT_FLOAT_EQ(m_light->light_power, 42.0f);
	EXPECT_TRUE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());
}

TEST_F(OperationManagerTest, Position_RoundTrip)
{
	m_eventBus.Enqueue(PositionChanged{ m_mesh->GetObjectID(), 1.0f, 2.0f, 3.0f });
	Process();
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(1.0f, 2.0f, 3.0f));

	m_operations.Undo();
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(0.0f, 0.0f, 0.0f));

	m_operations.Redo();
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(1.0f, 2.0f, 3.0f));
}

// --- Recording suppression during replay -----------------------------------

TEST_F(OperationManagerTest, Replay_DoesNotRecordNewOperations)
{
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 20.0f });
	Process();

	// Undo/Redo replay events synchronously through the same handler that
	// calls Submit(); the Replaying guard must suppress re-recording so the
	// stacks stay a single entry each and don't grow without bound.
	m_operations.Undo();
	EXPECT_FALSE(m_operations.CanUndo());
	EXPECT_TRUE(m_operations.CanRedo());

	m_operations.Redo();
	EXPECT_TRUE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());

	// A single undo returns to the recorded state (not stuck mid-stack).
	m_operations.Undo();
	EXPECT_FLOAT_EQ(m_light->light_power, 10.0f);
	EXPECT_FALSE(m_operations.CanUndo());
}

// --- Redo invalidation ------------------------------------------------------

TEST_F(OperationManagerTest, NewForwardOperation_ClearsRedo)
{
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 30.0f });
	Process();
	m_operations.Undo();
	ASSERT_TRUE(m_operations.CanRedo());

	// A fresh edit after an undo invalidates the redo branch.
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 55.0f });
	Process();
	EXPECT_FALSE(m_operations.CanRedo());
	EXPECT_TRUE(m_operations.CanUndo());
}

// --- Involution -------------------------------------------------------------

TEST_F(OperationManagerTest, Inverse_IsInvolution)
{
	auto op = std::make_unique<SetLightPowerOp>(m_light->GetObjectID(), 3.0f, 77.0f);
	auto twice = op->Inverse()->Inverse();

	// (g⁻¹)⁻¹ must reproduce the original forward effect.
	EXPECT_EQ(twice->Label(), op->Label());

	OperationContext ctx{ m_eventBus };
	twice->Apply(ctx); // Emit() dispatches synchronously.
	EXPECT_FLOAT_EQ(m_light->light_power, 77.0f);
}

TEST_F(OperationManagerTest, Inverse_SwapsBeforeAfter)
{
	m_light->SetPower(10.0f);
	auto op = std::make_unique<SetLightPowerOp>(m_light->GetObjectID(), 10.0f, 90.0f);
	auto inv = op->Inverse();

	OperationContext ctx{ m_eventBus };
	inv->Apply(ctx); // inverse applies the "before" value.
	EXPECT_FLOAT_EQ(m_light->light_power, 10.0f);
}

// --- Stale identity ---------------------------------------------------------

TEST_F(OperationManagerTest, Apply_UnknownUid_IsNoOp)
{
	const float before = m_light->light_power;
	auto op = std::make_unique<SetLightPowerOp>(999999, 1.0f, 2.0f); // no object with this UID

	OperationContext ctx{ m_eventBus };
	op->Apply(ctx); // Resolve() -> nullptr -> safe no-op, must not crash.
	EXPECT_FLOAT_EQ(m_light->light_power, before);
}

// --- Empty stacks -----------------------------------------------------------

TEST_F(OperationManagerTest, UndoRedo_EmptyStacks_AreNoOps)
{
	EXPECT_FALSE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());
	m_operations.Undo(); // no-op
	m_operations.Redo(); // no-op
	EXPECT_FALSE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());
}

TEST_F(OperationManagerTest, Clear_EmptiesBothStacks)
{
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 15.0f });
	Process();
	m_operations.Undo();
	ASSERT_TRUE(m_operations.CanRedo());

	m_operations.Clear();
	EXPECT_FALSE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());
}

// --- Visibility round-trip -------------------------------------------------

TEST_F(OperationManagerTest, Visibility_RoundTrip)
{
	// Mesh starts fully visible.
	ASSERT_TRUE(m_mesh->is_viewport);
	ASSERT_TRUE(m_mesh->is_rendered);

	m_eventBus.Enqueue(VisibilityChanged{ m_mesh->GetObjectID(), false, false });
	Process();
	EXPECT_FALSE(m_mesh->is_viewport);
	EXPECT_FALSE(m_mesh->is_rendered);

	m_operations.Undo();
	EXPECT_TRUE(m_mesh->is_viewport);
	EXPECT_TRUE(m_mesh->is_rendered);

	m_operations.Redo();
	EXPECT_FALSE(m_mesh->is_viewport);
	EXPECT_FALSE(m_mesh->is_rendered);
}

// --- Camera FOV ("Camera Ratio") round-trip --------------------------------

TEST_F(OperationManagerTest, CameraFov_RoundTrip)
{
	const float original = m_camera->cam_pers;

	m_eventBus.Enqueue(CameraFovChanged{ m_camera->GetObjectID(), 35.0f });
	Process();
	EXPECT_FLOAT_EQ(m_camera->cam_pers, 35.0f);

	m_operations.Undo();
	EXPECT_FLOAT_EQ(m_camera->cam_pers, original);

	m_operations.Redo();
	EXPECT_FLOAT_EQ(m_camera->cam_pers, 35.0f);
}

// --- Camera pose (coupled transform) round-trip ----------------------------

TEST_F(OperationManagerTest, CameraPose_RoundTrip)
{
	const glm::vec3 startPos(0.0f, 5.0f, 0.0f);
	const glm::vec3 startTar(0.0f, 0.0f, 0.0f);
	m_camera->SetPosition(startPos);
	m_camera->SetTarPos(startTar);

	const glm::vec3 endPos(2.0f, 5.0f, 1.0f);
	const glm::vec3 endTar(1.0f, 0.0f, 0.0f);

	// Simulate a recorded navigation: state already moved to end, op records it.
	m_camera->SetPosition(endPos);
	m_camera->SetTarPos(endTar);
	m_operations.Submit(std::make_unique<CameraTransformOp>(
		m_camera->GetObjectID(),
		CameraPose{ startPos, startTar },
		CameraPose{ endPos, endTar }));

	m_operations.Undo();
	EXPECT_EQ(m_camera->GetPosition(), startPos);
	EXPECT_EQ(m_camera->cam_tar, startTar);

	m_operations.Redo();
	EXPECT_EQ(m_camera->GetPosition(), endPos);
	EXPECT_EQ(m_camera->cam_tar, endTar);
}

// --- Merge coalescing -------------------------------------------------------

TEST_F(OperationManagerTest, CameraZoom_Merge_CoalescesIntoOneUndo)
{
	const int uid = m_camera->GetObjectID();
	const glm::vec3 a(0.0f, 5.0f, 0.0f);
	const glm::vec3 b(0.0f, 4.0f, 0.0f);
	const glm::vec3 c(0.0f, 3.0f, 0.0f);
	const glm::vec3 d(0.0f, 2.0f, 0.0f);
	const glm::vec3 tar(0.0f, 0.0f, 0.0f);

	// Three consecutive SAME-DIRECTION (zoom-in) edits share a MergeKey and
	// collapse into a single undo entry spanning a→d.
	m_operations.Submit(std::make_unique<CameraZoomOp>(uid, CameraPose{ a, tar }, CameraPose{ b, tar }));
	m_operations.Submit(std::make_unique<CameraZoomOp>(uid, CameraPose{ b, tar }, CameraPose{ c, tar }));
	m_operations.Submit(std::make_unique<CameraZoomOp>(uid, CameraPose{ c, tar }, CameraPose{ d, tar }));

	ASSERT_TRUE(m_operations.CanUndo());

	// A single undo returns to the original endpoint, proving one merged entry.
	m_operations.Undo();
	EXPECT_EQ(m_camera->GetPosition(), a);
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(OperationManagerTest, CameraZoom_DirectionChange_BreaksMerge)
{
	const int uid = m_camera->GetObjectID();
	const glm::vec3 a(0.0f, 5.0f, 0.0f);
	const glm::vec3 b(0.0f, 4.0f, 0.0f);
	const glm::vec3 c(0.0f, 3.0f, 0.0f);
	const glm::vec3 tar(0.0f, 0.0f, 0.0f);

	// Zoom in twice, then zoom back OUT: the direction change must break the
	// merge, producing two undo entries (merged zoom-in run + zoom-out run).
	m_operations.Submit(std::make_unique<CameraZoomOp>(uid, CameraPose{ a, tar }, CameraPose{ b, tar }));
	m_operations.Submit(std::make_unique<CameraZoomOp>(uid, CameraPose{ b, tar }, CameraPose{ c, tar }));
	m_operations.Submit(std::make_unique<CameraZoomOp>(uid, CameraPose{ c, tar }, CameraPose{ b, tar }));

	ASSERT_TRUE(m_operations.CanUndo());

	// Two undos empty the stack: the zoom-out, then the merged zoom-in.
	m_operations.Undo();
	EXPECT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(OperationManagerTest, CameraTransform_DoesNotMerge_SeparateEntries)
{
	const int uid = m_camera->GetObjectID();
	const glm::vec3 a(0.0f, 5.0f, 0.0f);
	const glm::vec3 b(0.0f, 4.0f, 0.0f);
	const glm::vec3 c(0.0f, 3.0f, 0.0f);
	const glm::vec3 tar(0.0f, 0.0f, 0.0f);

	// CameraTransformOp (drag commit) is non-mergeable: three separate ops on
	// the SAME camera stay three distinct undo entries — the drag-undo bug fix.
	m_operations.Submit(std::make_unique<CameraTransformOp>(uid, CameraPose{ a, tar }, CameraPose{ b, tar }));
	m_operations.Submit(std::make_unique<CameraTransformOp>(uid, CameraPose{ b, tar }, CameraPose{ c, tar }));
	m_operations.Submit(std::make_unique<CameraTransformOp>(uid, CameraPose{ c, tar }, CameraPose{ a, tar }));

	// Three undos to clear (a←c, c←b, b←a), one entry each.
	ASSERT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_EQ(m_camera->GetPosition(), c);
	ASSERT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_EQ(m_camera->GetPosition(), b);
	ASSERT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_EQ(m_camera->GetPosition(), a);
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(OperationManagerTest, DifferentKeys_DoNotMerge)
{
	m_eventBus.Enqueue(CameraFovChanged{ m_camera->GetObjectID(), 40.0f });
	Process();
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 25.0f });
	Process();

	// Two different-key edits are two entries: two undos to clear.
	ASSERT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(OperationManagerTest, CameraTransform_Inverse_IsInvolution)
{
	const int uid = m_camera->GetObjectID();
	const glm::vec3 pos(1.0f, 2.0f, 3.0f);
	const glm::vec3 tar(4.0f, 5.0f, 6.0f);
	auto op = std::make_unique<CameraTransformOp>(
		uid, CameraPose{ glm::vec3(0.0f), glm::vec3(0.0f) }, CameraPose{ pos, tar });
	auto twice = op->Inverse()->Inverse();

	EXPECT_EQ(twice->Label(), op->Label());
	EXPECT_EQ(twice->MergeKey(), op->MergeKey());

	OperationContext ctx{ m_eventBus };
	twice->Apply(ctx);
	EXPECT_EQ(m_camera->GetPosition(), pos);
	EXPECT_EQ(m_camera->cam_tar, tar);
}

// --- Selection undo/redo ----------------------------------------------------

TEST_F(OperationManagerTest, Selection_RoundTrip)
{
	// Select the mesh.
	m_eventBus.Enqueue(ObjectSelected{ m_mesh->GetObjectID(), 0 });
	Process();
	EXPECT_TRUE(m_scene.selections.IsSelected(m_mesh.get()));
	EXPECT_EQ(m_scene.selections.GetActiveObject(), m_mesh.get());
	ASSERT_TRUE(m_operations.CanUndo());

	// Undo clears the selection.
	m_operations.Undo();
	EXPECT_FALSE(m_scene.selections.IsSelected(m_mesh.get()));
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 0u);

	// Redo restores it.
	m_operations.Redo();
	EXPECT_TRUE(m_scene.selections.IsSelected(m_mesh.get()));
	EXPECT_EQ(m_scene.selections.GetActiveObject(), m_mesh.get());
}

TEST_F(OperationManagerTest, Selection_MultiSelect_RoundTrip)
{
	// Select mesh, then shift-add the light.
	m_eventBus.Enqueue(ObjectSelected{ m_mesh->GetObjectID(), 0 });
	Process();
	m_eventBus.Enqueue(ObjectSelected{ m_light->GetObjectID(), Input::Mod_Shift });
	Process();
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 2u);
	EXPECT_TRUE(m_scene.selections.IsSelected(m_mesh.get()));
	EXPECT_TRUE(m_scene.selections.IsSelected(m_light.get()));

	// Undo removes only the multi-select step (back to mesh only).
	m_operations.Undo();
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 1u);
	EXPECT_TRUE(m_scene.selections.IsSelected(m_mesh.get()));
	EXPECT_FALSE(m_scene.selections.IsSelected(m_light.get()));

	// Redo re-adds the light.
	m_operations.Redo();
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 2u);
	EXPECT_TRUE(m_scene.selections.IsSelected(m_light.get()));
}

TEST_F(OperationManagerTest, Selection_DoesNotClearRedo)
{
	// Record a real edit, then undo it so a redo is pending.
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 42.0f });
	Process();
	m_operations.Undo();
	EXPECT_FLOAT_EQ(m_light->light_power, 10.0f);
	ASSERT_TRUE(m_operations.CanRedo());

	// A selection change is transparent: it appends to undo but MUST keep the
	// pending redo intact (the key property of PreservesRedo()).
	m_eventBus.Enqueue(ObjectSelected{ m_mesh->GetObjectID(), 0 });
	Process();
	EXPECT_TRUE(m_scene.selections.IsSelected(m_mesh.get()));
	ASSERT_TRUE(m_operations.CanRedo());

	// Redo still restores the undone edit.
	m_operations.Redo();
	EXPECT_FLOAT_EQ(m_light->light_power, 42.0f);
}

TEST_F(OperationManagerTest, RealEdit_ClearsRedo_AfterSelection)
{
	// Undo a real edit (redo pending), interleave a selection, then a real edit.
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 42.0f });
	Process();
	m_operations.Undo();
	ASSERT_TRUE(m_operations.CanRedo());

	m_eventBus.Enqueue(ObjectSelected{ m_mesh->GetObjectID(), 0 });
	Process();
	ASSERT_TRUE(m_operations.CanRedo()); // selection preserved redo

	// A branching edit still discards the redo timeline.
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 88.0f });
	Process();
	EXPECT_FALSE(m_operations.CanRedo());
}

TEST_F(OperationManagerTest, SetSelectionOp_Inverse_IsInvolution)
{
	SelectionState before{ {}, 0 };
	SelectionState after{ { m_mesh->GetObjectID() }, m_mesh->GetObjectID() };
	auto op = std::make_unique<SetSelectionOp>(before, after);
	auto twice = op->Inverse()->Inverse();

	EXPECT_EQ(twice->Label(), op->Label());
	EXPECT_TRUE(twice->PreservesRedo());

	OperationContext ctx{ m_eventBus };
	twice->Apply(ctx); // reproduces the original "after" selection.
	EXPECT_TRUE(m_scene.selections.IsSelected(m_mesh.get()));
	EXPECT_EQ(m_scene.selections.GetActiveObject(), m_mesh.get());
}

// --- Bounded undo depth -----------------------------------------------------

TEST_F(OperationManagerTest, BoundedUndoDepth_EvictsOldestEntries)
{
	// A manager capped at 3 entries: submitting 5 distinct edits keeps only the
	// newest 3; the two oldest are evicted from the front.
	OperationManager capped{ m_eventBus, 3 };

	const int uid = m_light->GetObjectID();
	for (int i = 1; i <= 5; ++i)
		capped.Submit(std::make_unique<SetLightPowerOp>(
			uid, static_cast<float>(i), static_cast<float>(i + 1)));

	const HistoryView view = capped.GetHistoryView();
	ASSERT_EQ(view.undo.size(), 3u); // only the newest 3 survive

	// Exactly three undos exhaust the (bounded) stack — no more, no less.
	capped.Undo();
	capped.Undo();
	capped.Undo();
	EXPECT_FALSE(capped.CanUndo());
}

// --- Replay exception safety ------------------------------------------------

namespace {

/** @brief Operation whose Apply always throws — exercises replay phase cleanup. */
class ThrowingOp : public Operation
{
public:
	void Apply(OperationContext&) override { throw std::runtime_error("boom"); }
	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<ThrowingOp>();
	}
	std::string Label() const override { return "Throwing"; }
};

} // anonymous namespace

TEST_F(OperationManagerTest, ExceptionDuringUndo_DoesNotKillRecording)
{
	// A throwing op sits on top of the stack.
	m_operations.Submit(std::make_unique<ThrowingOp>());
	ASSERT_TRUE(m_operations.CanUndo());

	// Replay contains handler exceptions: Undo() must NOT throw, the failed
	// inverse must NOT be pushed to redo, and the replay phase is restored —
	// otherwise every later Submit() is silently suppressed and the whole undo
	// system appears dead.
	m_operations.Undo();
	ASSERT_FALSE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo()); // failed op dropped, not moved to redo

	// A fresh edit must still record after the failed undo.
	m_eventBus.Enqueue(LightPowerChanged{ m_light->GetObjectID(), 7.0f });
	Process();
	EXPECT_TRUE(m_operations.CanUndo());

	// And undo of the fresh edit still applies correctly.
	m_operations.Undo();
	EXPECT_FLOAT_EQ(m_light->light_power, 10.0f);
}

TEST_F(OperationManagerTest, ExceptionDuringRedo_DoesNotPushToUndo)
{
	// Seed a throwing op on the redo stack (as a failed inverse would be).
	std::vector<std::unique_ptr<Operation>> redo;
	redo.push_back(std::make_unique<ThrowingOp>());
	m_operations.RestoreHistory({}, std::move(redo));
	ASSERT_TRUE(m_operations.CanRedo());

	// Redo must not throw, and the failed forward op must not be pushed to undo.
	m_operations.Redo();
	EXPECT_FALSE(m_operations.CanRedo());
	EXPECT_FALSE(m_operations.CanUndo());
}

// --- Script scope: one run, one undo step -----------------------------------

TEST_F(OperationManagerTest, ScriptScopeCompactsIntoOneEntry)
{
	// The mesh is pooled, so the composite's Undo replays into a real object: the
	// run is one entry that rewinds as a whole, not merely one stack slot.
	const int uid = m_mesh->GetObjectID();
	m_operations.BeginScriptScope();
	m_operations.Submit(std::make_unique<SetPositionOp>(uid, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
	m_operations.Submit(
		std::make_unique<SetPositionOp>(uid, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f)));
	m_operations.Submit(
		std::make_unique<SetPositionOp>(uid, glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(3.0f, 0.0f, 0.0f)));
	m_mesh->SetPosition(glm::vec3(3.0f, 0.0f, 0.0f)); // where the run left the object
	m_operations.EndScriptScope();

	ASSERT_EQ(m_operations.GetUndoStack().size(), 1u);
	EXPECT_TRUE(m_operations.CanUndo());
	EXPECT_FALSE(m_operations.CanRedo());

	m_operations.Undo();
	EXPECT_TRUE(m_operations.GetUndoStack().empty());
	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(0.0f));
}

/**
 * @test A script write to a mergeable property must not fold into the entry the
 *       previous live edit left on the stack: the run boundary stops coalescing
 *       even when the MergeKey matches exactly.
 */
TEST_F(OperationManagerTest, ScriptScopeDoesNotMergeWithThePreviousEntry)
{
	const int uid = m_camera->GetObjectID();
	const float original = m_camera->cam_pers;

	// A live panel edit on the same camera, same op type, same MergeKey.
	m_camera->ChangeCamPersp(40.0f);
	m_operations.Submit(std::make_unique<CameraFovOp>(uid, original, 40.0f));

	m_operations.BeginScriptScope();
	m_camera->ChangeCamPersp(55.0f);
	m_operations.Submit(std::make_unique<CameraFovOp>(uid, 40.0f, 55.0f));
	m_operations.EndScriptScope();

	ASSERT_EQ(m_operations.GetUndoStack().size(), 2u);

	// One undo leaves the first entry's recorded end state (40), not 55: it kept
	// its own value instead of swallowing the script write.
	m_operations.Undo();
	EXPECT_FLOAT_EQ(m_camera->cam_pers, 40.0f);
	ASSERT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_FLOAT_EQ(m_camera->cam_pers, original);
}

/**
 * @test A script run names itself in the History panel: the default "Run Script"
 *       on the undo list, and again on the redo list after Undo - never the first
 *       child's label.
 */
TEST_F(OperationManagerTest, ScriptRun_IsLabelledInUndoAndRedo)
{
	m_operations.BeginScriptScope();
	m_operations.Submit(std::make_unique<SetPositionOp>(
		m_mesh->GetObjectID(), glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
	m_operations.EndScriptScope();

	ASSERT_EQ(m_operations.GetHistoryView().undo.size(), 1u);
	EXPECT_EQ(m_operations.GetHistoryView().undo.back(), "Run Script");

	m_operations.Undo();
	ASSERT_EQ(m_operations.GetHistoryView().redo.size(), 1u);
	EXPECT_EQ(m_operations.GetHistoryView().redo.front(), "Run Script");
}

/**
 * @test The host's name is the history label, and the redo entry keeps it: a file
 *       run passes "Run <file>" so it cannot be mistaken for a console run.
 */
TEST_F(OperationManagerTest, ScriptRun_UsesTheHostSuppliedName)
{
	m_operations.BeginScriptScope("Run demo.py");
	m_operations.Submit(std::make_unique<SetPositionOp>(
		m_mesh->GetObjectID(), glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
	m_operations.EndScriptScope();

	ASSERT_EQ(m_operations.GetHistoryView().undo.size(), 1u);
	EXPECT_EQ(m_operations.GetHistoryView().undo.back(), "Run demo.py");

	m_operations.Undo();
	ASSERT_EQ(m_operations.GetHistoryView().redo.size(), 1u);
	EXPECT_EQ(m_operations.GetHistoryView().redo.front(), "Run demo.py");
}

/**
 * @test A composite that is not a script run still reports its first child's
 *       label, so existing History text (an add or delete step) does not change.
 */
TEST_F(OperationManagerTest, Composite_KeepsTheFirstChildLabel)
{
	std::vector<std::unique_ptr<Operation>> edits;
	edits.push_back(std::make_unique<SetPositionOp>(
		m_mesh->GetObjectID(), glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
	edits.push_back(std::make_unique<SetRotationOp>(
		m_mesh->GetObjectID(), glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 45.0f)));
	m_operations.Submit(std::make_unique<CompositeOp>(std::move(edits)));

	ASSERT_EQ(m_operations.GetHistoryView().undo.size(), 1u);
	EXPECT_EQ(m_operations.GetHistoryView().undo.back(), "Move");
}

TEST_F(OperationManagerTest, EmptyScriptScopeCreatesNoEntry)
{
	m_operations.BeginScriptScope();
	m_operations.EndScriptScope();
	EXPECT_TRUE(m_operations.GetUndoStack().empty());
}

TEST_F(OperationManagerTest, ScriptScopeRecoversFromAFailedRun)
{
	m_operations.BeginScriptScope();
	m_operations.Submit(std::make_unique<SetPositionOp>(kScriptUid, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
	m_operations.EndScriptScope(); // a raising run still calls this

	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u);
	EXPECT_TRUE(m_operations.GetUndoStack().back() != nullptr);
}

TEST_F(OperationManagerTest, Clear_DropsAnOpenScriptBuffer)
{
	m_operations.BeginScriptScope();
	m_operations.Submit(std::make_unique<SetPositionOp>(kScriptUid, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));

	// New/Load wipes the history mid-scope: the buffered op must go with it
	// instead of surfacing as a step in the next document.
	m_operations.Clear();
	m_operations.EndScriptScope();

	EXPECT_TRUE(m_operations.GetUndoStack().empty());
	EXPECT_FALSE(m_operations.CanUndo());
}

