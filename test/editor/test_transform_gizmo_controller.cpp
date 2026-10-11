/**
 * @file test_transform_gizmo_controller.cpp
 * @brief Pins the gizmo controller's notification tiers, which nothing else can see.
 *
 * test_transform_gizmo.cpp covers the state machine: what a drag does to a
 * Transform3D. This file covers the other half of every handler — the events it
 * enqueues — because those are invisible in a screenshot and wrong in a way that
 * only shows up much later:
 *
 *   1. **A cancelled gesture leaves no trace.** `SceneModified` is one-way: nothing
 *      un-dirties a project. A drag that emitted it would leave File > Save lit up
 *      for a change the user already abandoned, and no later event can take it back.
 *      So a whole G-drag-Esc round trip must emit zero `SceneModified`, and the
 *      transform must be back at its starting value.
 *   2. **A confirmed gesture is exactly one undo entry and exactly one dirty mark**,
 *      no matter how many drag frames it spans. Submitting per frame would bury the
 *      undo stack (the three transform ops declare no MergeKey, so nothing
 *      coalesces).
 *   3. **A light syncs cheaply while dragging and authoritatively on commit.** The
 *      light's GPU position lives in an SSBO the Transform3D write does not touch,
 *      so a drag that emitted neither `LightGpuChanged` nor `LightingRebuild` would
 *      render a light that visibly lags its own guide.
 *   4. **A mesh asks for no light work at all.** The cheap path and the full dict
 *      rebuild are both wasted on a mesh, and the dict one walks every light.
 *   5. **A gesture that ends where it started records nothing.** A stray G-Enter
 *      must not litter the undo stack with a no-op.
 *
 * No GPU and no Editor: a bare EventQueue with counting subscribers stands in for
 * the Editor's own subscriptions, which is all the controller can observe anyway.
 */

#include <gtest/gtest.h>

#include <memory>

#include "editor/controllers/TransformGizmoController.h"
#include "editor/events/EditorEvents.h"
#include "editor/events/EventBus.h"
#include "editor/events/GizmoEvents.h"
#include "editor/events/InputEvents.h"
#include "editor/operations/OperationManager.h"
#include "editor/viewport/EditorViewport.h"
#include "editor/viewport/TransformGizmo.h"
#include "core/ResourceManager.h"
#include "core/RenderConfig.h"
#include "scene/Camera.h"
#include "scene/DebugLine.h"
#include "scene/DebugPoints.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

using namespace neurus;

namespace {

constexpr uint32_t kW = 800u;
constexpr uint32_t kH = 600u;

/// @brief What the Editor would have done with the controller's output.
struct EventTally
{
	int modified = 0;   ///< SceneModified: the project needs saving.
	int reset = 0;      ///< RenderResetEvent: redraw / reflatten the overlays.
	int lightStruct = 0;///< LightGpuChanged: one light's SSBO entry.
	int lightDict = 0;  ///< LightingRebuild: the whole light SSBO dict.
};

} // namespace

/**
 * @brief Camera at (0,-5,0) looking at the origin, Z up — the project convention.
 *
 * Same pose as test_gizmo_draw_builder.cpp, so screen right is world +X and a
 * horizontal cursor sweep is a nameable world direction.
 */
class TransformGizmoControllerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_cam.ChangeCamRatio(static_cast<float>(kW), static_cast<float>(kH));
		m_cam.SetPosition(glm::vec3(0.0f, -5.0f, 0.0f));
		m_cam.SetTarPos(glm::vec3(0.0f));
		m_viewport.SetViewportSize(glm::uvec2(kW, kH), glm::uvec2(kW, kH));
		m_viewport.SetCamera(&m_cam);

		m_mesh = std::make_shared<Mesh>();
		m_light = std::make_shared<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
		m_scene.UseMesh(m_mesh);
		m_scene.UseLight(m_light);

		m_controller.Init(m_ctx);

		// Registered after the controller, so these see everything it enqueues --
		// including the events it enqueues re-entrantly during Process().
		m_eventBus.subscribe<SceneModified>([this](const SceneModified&) { ++m_tally.modified; });
		m_eventBus.subscribe<RenderResetEvent>([this](const RenderResetEvent&) { ++m_tally.reset; });
		m_eventBus.subscribe<LightGpuChanged>([this](const LightGpuChanged&) { ++m_tally.lightStruct; });
		m_eventBus.subscribe<LightingRebuild>([this](const LightingRebuild&) { ++m_tally.lightDict; });
	}

	/// @brief Arms @p mode on @p uid with the cursor parked at the screen centre.
	void Arm(GizmoMode mode, int uid)
	{
		m_viewport.SetCursor(glm::vec2(kW * 0.5f, kH * 0.5f));
		m_eventBus.enqueue(GizmoModeRequested{uid, mode});
		m_eventBus.Process();
	}

	/// @brief One drag frame to @p x pixels along the screen's horizontal centre.
	void DragTo(float x)
	{
		MouseMoveEvent move{};
		move.position = glm::vec2(x, kH * 0.5f);
		m_eventBus.enqueue(move);
		m_eventBus.Process();
	}

	void Confirm()
	{
		m_eventBus.enqueue(GizmoConfirmed{});
		m_eventBus.Process();
	}

	void Cancel()
	{
		m_eventBus.enqueue(GizmoCancelled{});
		m_eventBus.Process();
	}

	Transform3D& MeshTransform() { return *static_cast<Transform3D*>(m_mesh->GetTransform()); }
	Transform3D& LightTransform() { return *static_cast<Transform3D*>(m_light->GetTransform()); }

	EventQueue m_eventBus;
	Scene m_scene;
	OperationManager m_operations{ m_eventBus };
	ResourceManager m_resources;
	RenderConfig m_config;
	Camera m_cam;
	EditorViewport m_viewport;
	ControllerContext m_ctx{ m_eventBus, m_resources, m_operations,
	                         [this]() { return &m_scene; },
	                         [this]() { return &m_config; },
	                         [this]() { return &m_viewport; } };
	TransformGizmoController m_controller;
	std::shared_ptr<Mesh> m_mesh;
	std::shared_ptr<Light> m_light;
	EventTally m_tally{};
};

// --- 1. A cancelled gesture leaves no trace --------------------------------

/// @test G, drag, Esc: the transform is restored and the project is never dirtied.
TEST_F(TransformGizmoControllerTest, CancelledGestureNeverDirtiesTheProject)
{
	const glm::vec3 start = MeshTransform().GetPosition();

	Arm(GizmoMode::Move, m_mesh->GetObjectID());
	DragTo(kW * 0.75f);
	DragTo(kW * 0.90f);
	ASSERT_NE(MeshTransform().GetPosition(), start) << "the drag must have moved something";

	Cancel();

	EXPECT_EQ(MeshTransform().GetPosition(), start);
	EXPECT_EQ(m_tally.modified, 0)
	    << "SceneModified is one-way: a drag that emits it leaves the project dirty "
	       "for a change the user abandoned";
	EXPECT_EQ(m_operations.GetUndoStack().size(), 0u) << "a cancelled gesture records no op";
	EXPECT_GT(m_tally.reset, 0) << "the guide still has to appear, move and vanish";
}

/// @test Re-arming mid-gesture (G then R) cancels the first without dirtying.
TEST_F(TransformGizmoControllerTest, ReArmingMidGestureStaysTransient)
{
	const glm::vec3 start = MeshTransform().GetPosition();

	Arm(GizmoMode::Move, m_mesh->GetObjectID());
	DragTo(kW * 0.75f);
	Arm(GizmoMode::Rotate, m_mesh->GetObjectID());

	EXPECT_EQ(MeshTransform().GetPosition(), start) << "the abandoned move was undone";
	EXPECT_EQ(m_tally.modified, 0);
	EXPECT_EQ(m_operations.GetUndoStack().size(), 0u);

	Cancel();
	EXPECT_EQ(m_tally.modified, 0);
}

// --- 2. A confirmed gesture is one op and one dirty mark -------------------

/// @test Many drag frames, one undo entry, one SceneModified.
TEST_F(TransformGizmoControllerTest, ConfirmedGestureIsOneOpAndOneDirtyMark)
{
	Arm(GizmoMode::Move, m_mesh->GetObjectID());
	DragTo(kW * 0.60f);
	DragTo(kW * 0.70f);
	DragTo(kW * 0.80f);
	EXPECT_EQ(m_tally.modified, 0) << "still revertible while the gesture is live";
	EXPECT_EQ(m_operations.GetUndoStack().size(), 0u) << "the drag writes Transform3D directly";

	Confirm();

	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u) << "one gesture, one undo entry";
	EXPECT_EQ(m_tally.modified, 1) << "one gesture, one dirty mark";
}

// --- 3/4. Light work is type-correct and tier-correct ----------------------

/// @test Dragging a light syncs the cheap single struct, never the dict.
TEST_F(TransformGizmoControllerTest, DraggingALightUsesTheCheapSync)
{
	Arm(GizmoMode::Move, m_light->GetObjectID());
	DragTo(kW * 0.70f);
	DragTo(kW * 0.80f);

	EXPECT_EQ(m_tally.lightStruct, 2) << "one cheap SSBO update per drag frame";
	EXPECT_EQ(m_tally.lightDict, 0) << "a full dict rebuild walks every light: not per frame";
	EXPECT_EQ(m_tally.modified, 0);
}

/// @test Confirming a light's move rebuilds the dict, which is what filters variants.
TEST_F(TransformGizmoControllerTest, ConfirmingALightRebuildsTheDict)
{
	Arm(GizmoMode::Move, m_light->GetObjectID());
	DragTo(kW * 0.70f);
	Confirm();

	EXPECT_EQ(m_tally.lightDict, 1);
	EXPECT_EQ(m_tally.modified, 1);
}

// --- 6. Debug line/points are ordinary gizmo targets ------------------------

/**
 * @test A DebugLine is gizmo-targetable, exactly like a mesh.
 *
 * DebugLine/DebugPoints already inherit Transform3D and DebugDrawBuilder already
 * bakes their matrix into world space, but neither overrode GetTransform(), so
 * the gizmo could not arm on them (the resting handle still drew, via a separate
 * dynamic_cast). This pins that G/R/S now moves them and a confirm records
 * exactly one op and one dirty mark.
 */
TEST_F(TransformGizmoControllerTest, DebugLineIsGizmoTargetable)
{
	auto line = std::make_shared<DebugLine>();
	line->PushDebugLine(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	m_scene.UseDebugLine(line);

	auto* xf = static_cast<Transform3D*>(line->GetTransform());
	ASSERT_NE(xf, nullptr) << "a DebugLine must expose a transform to be targetable";
	const glm::vec3 start = xf->GetPosition();

	Arm(GizmoMode::Move, line->GetObjectID());
	DragTo(kW * 0.75f);

	EXPECT_NE(xf->GetPosition(), start) << "the drag must move the debug line";
	EXPECT_EQ(m_tally.modified, 0) << "still revertible while the gesture is live";

	Confirm();
	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u) << "one gesture, one undo entry";
	EXPECT_EQ(m_tally.modified, 1) << "one gesture, one dirty mark";
}

/// @test A mesh asks for no light work in either tier.
TEST_F(TransformGizmoControllerTest, MeshGestureAsksForNoLightWork)
{
	Arm(GizmoMode::Move, m_mesh->GetObjectID());
	DragTo(kW * 0.70f);
	Confirm();

	EXPECT_EQ(m_tally.lightStruct, 0);
	EXPECT_EQ(m_tally.lightDict, 0);
}

// --- 5. A no-op gesture records nothing ------------------------------------

/// @test G then Enter with no cursor motion: dirty, but no undo entry.
TEST_F(TransformGizmoControllerTest, GestureThatMovedNothingRecordsNoOp)
{
	Arm(GizmoMode::Move, m_mesh->GetObjectID());
	Confirm();

	EXPECT_EQ(m_operations.GetUndoStack().size(), 0u)
	    << "a stray G-Enter must not litter the undo stack";
}
