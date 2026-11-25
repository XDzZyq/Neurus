/**
 * @file test_camera_controller.cpp
 * @brief Tests for CameraController event-driven navigation (orbit, dolly, pan, zoom).
 *
 * CameraController is now event-driven 鈥?it subscribes to CameraEvents via Init()
 * and reacts to enqueued events dispatched through EventQueue::Process().
 *
 * MMB Convention (from decisions.md):
 *   - MMB alone       = Orbit  (CameraRotateEvent)
 *   - Ctrl + MMB      = Dolly  (CameraPushEvent)
 *   - Shift + MMB     = Pan    (CameraSlideEvent)
 *   - Scroll wheel    = Zoom   (CameraZoomEvent)
 *
 * Sensitivity constants (from CameraController.hpp):
 *   - kOrbitSensitivity = 0.005f  (radians per pixel)
 *   - kPanSensitivity   = 0.01f   (world units per pixel)
 *   - kZoomSensitivity  = 1.0f    (factor per scroll notch)
 *   - kDollySensitivity = 0.05f   (world units per pixel)
 */

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "editor/controllers/CameraController.hpp"
#include "editor/events/CameraEvents.hpp"
#include "editor/operations/OperationManager.hpp"
#include "asset/ResourceManager.hpp"
#include "scene/Camera.hpp"
#include "scene/Scene.hpp"
#include "core/RenderConfig.hpp"

using namespace neurus;

namespace {

/**
 * @brief Computes Euclidean distance from camera to its look-at target.
 */
float DistanceToTarget(const Camera& cam)
{
	return glm::length(cam.GetPosition() - cam.cam_tar);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test Fixture
// ---------------------------------------------------------------------------

class CameraControllerTest : public ::testing::Test
{
	protected:
	void SetUp() override
	{
		// Created through the pool, not make_shared: CameraController resolves a
		// camId through ControllerContext::resources, so a camera that is not
		// registered there cannot be navigated. Every camera in the real app is
		// born the same way (Editor.cpp, SceneComponent.cpp).
		m_camera = m_resources.Load<Camera>();
		m_scene.UseCamera(m_camera);
		m_scene.ActivateCamera(m_camera->GetObjectID());
		m_controller = std::make_unique<CameraController>();
		m_controller->Init(m_ctx);
	}

	void TearDown() override
	{
		// No-op — each test fixture has its own EventQueue, GC'd on teardown
	}

	EventQueue m_eventBus;
	Scene m_scene;
	OperationManager m_operations{ m_eventBus };
	ResourceManager m_resources;
	RenderConfig m_config;
	ControllerContext m_ctx{ m_eventBus, m_resources, m_operations,
	                         [this]() { return &m_scene; },
	                         [this]() { return &m_config; } };
	std::shared_ptr<Camera> m_camera;
	std::unique_ptr<CameraController> m_controller;
};

// ===========================================================================
// Orbit tests (MMB, no modifier 鈫?CameraRotateEvent)
// ===========================================================================

/** @test OrbitChangesAzimuthAndElevationAroundFixedTarget */
TEST_F(CameraControllerTest, OrbitChangesAzimuthAndElevationAroundFixedTarget)
{
	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f));
	m_eventBus.Enqueue(CameraRotateEvent{m_camera->GetObjectID(), 10.0f, 0.0f});
	m_eventBus.Process();
	EXPECT_LT(m_camera->GetPosition().y, -1e-4f);
	EXPECT_FLOAT_EQ(m_camera->GetPosition().z, 0.0f);
	EXPECT_EQ(m_camera->cam_tar, glm::vec3(0.0f));

	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_eventBus.Enqueue(CameraRotateEvent{m_camera->GetObjectID(), 0.0f, -10.0f});
	m_eventBus.Process();
	EXPECT_NEAR(m_camera->GetPosition().y, 0.0f, 1e-4f);
	EXPECT_LT(m_camera->GetPosition().z, 0.0f);
	EXPECT_EQ(m_camera->cam_tar, glm::vec3(0.0f));
}

/**
 * @test Orbit_Clamp_Elevation_89_Degrees
 *
 * Camera at (0,0,5) looking straight down at origin (Z-up convention).
 * A large mouseDeltaY should NOT flip the camera past the poles.
 * Elevation is clamped to 卤89掳. No crash, position values remain finite.
 */
TEST_F(CameraControllerTest, Orbit_Clamp_Elevation_89_Degrees)
{
	m_camera->SetPosition(glm::vec3(0.0f, 0.0f, 5.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	CameraRotateEvent e{m_camera->GetObjectID(), 0.0f, 1000.0f};

	// Should not crash or assert
	EXPECT_NO_FATAL_FAILURE(
		m_eventBus.Enqueue(e);
		m_eventBus.Process();
	);

	const glm::vec3 pos = m_camera->GetPosition();
	EXPECT_TRUE(std::isfinite(pos.x));
	EXPECT_TRUE(std::isfinite(pos.y));
	EXPECT_TRUE(std::isfinite(pos.z));

	// Elevation from target should not exceed 89 (Z-up: elevation = asin(dir.z))
	const glm::vec3 dir = pos - m_camera->cam_tar;
	const float radius = glm::length(dir);
	if (radius > 1e-6f)
	{
		const float elevation = std::asin(dir.z / radius);
		EXPECT_LE(std::abs(elevation), glm::radians(89.1f));
	}
}

// ===========================================================================
// Zoom tests (scroll wheel 鈫?CameraZoomEvent)
// ===========================================================================

/** @test WheelDirectionControlsZoomDistance */
TEST_F(CameraControllerTest, WheelDirectionControlsZoomDistance)
{
	for (float direction : { 1.0f, -1.0f })
	{
		SCOPED_TRACE(direction);
		m_camera->SetPosition(glm::vec3(0.0f, 5.0f, 0.0f));
		m_camera->SetTarPos(glm::vec3(0.0f));
		const float distance = DistanceToTarget(*m_camera);
		m_eventBus.Enqueue(CameraZoomEvent{m_camera->GetObjectID(), direction});
		m_eventBus.Process();
		EXPECT_LT((DistanceToTarget(*m_camera) - distance) * direction, 0.0f);
		EXPECT_EQ(m_camera->cam_tar, glm::vec3(0.0f));
	}
	m_camera->SetPosition(glm::vec3(0.0f, 0.01f, 0.0f));
	m_eventBus.Enqueue(CameraZoomEvent{m_camera->GetObjectID(), 1.0f});
	m_eventBus.Process();
	EXPECT_GE(DistanceToTarget(*m_camera), 0.01f);
}

// ===========================================================================
// Dolly tests (Ctrl + MMB 鈫?CameraPushEvent)
// ===========================================================================

/**
 * @test Dolly_CtrlMMB_MovesAlongForward
 *
 * Camera at (0,5,0), target at origin (Z-up: +Y is forward).
 * Ctrl+MMB drag up (positive mouseDeltaY) should move camera along
 * the forward/view direction (Y axis). X unchanged.
 * Target stays fixed (dolly translates only camera, not target).
 */
TEST_F(CameraControllerTest, Dolly_CtrlMMB_MovesAlongForward)
{
	m_camera->SetPosition(glm::vec3(0.0f, 5.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const float initialX = m_camera->GetPosition().x;
	const float initialY = m_camera->GetPosition().y;
	const glm::vec3 initialTar = m_camera->cam_tar;

	CameraPushEvent e{m_camera->GetObjectID(), 0.0f, 10.0f};
	m_eventBus.Enqueue(e);
	m_eventBus.Process();

	// Camera moved along forward axis (Y changed)
	EXPECT_NE(m_camera->GetPosition().y, initialY);
	// X unchanged (movement is strictly along view direction)
	EXPECT_FLOAT_EQ(m_camera->GetPosition().x, initialX);
	// Target stays fixed
	EXPECT_EQ(m_camera->cam_tar, initialTar);
}

// ===========================================================================
// Pan tests (Shift + MMB 鈫?CameraSlideEvent)
// ===========================================================================

/** @test PanTranslatesEyeAndTargetTogetherInBothDirections */
TEST_F(CameraControllerTest, PanTranslatesEyeAndTargetTogetherInBothDirections)
{
	const glm::vec3 initialPos(5.0f, 0.0f, 0.0f);
	const glm::vec3 initialTar(0.0f);
	const struct { float dx, dy; glm::vec3 translation; } cases[] = {
		{10.0f, 0.0f, {0.0f, -0.1f, 0.0f}},
		{0.0f, 10.0f, {0.0f, 0.0f, 0.1f}},
	};
	for (const auto& [dx, dy, translation] : cases)
	{
		SCOPED_TRACE(::testing::Message() << "dx=" << dx << ", dy=" << dy);
		m_camera->SetPosition(initialPos);
		m_camera->SetTarPos(initialTar);
		m_eventBus.Enqueue(CameraSlideEvent{m_camera->GetObjectID(), dx, dy});
		m_eventBus.Process();
		EXPECT_NEAR(glm::distance(m_camera->GetPosition(), initialPos + translation), 0.0f, 1e-4f);
		EXPECT_NEAR(glm::distance(m_camera->cam_tar, initialTar + translation), 0.0f, 1e-4f);
	}
}

// ===========================================================================
// Edge case tests
// ===========================================================================

/**
 * @test Edge_CameraAtTarget_NoCrash
 *
 * Camera position equals target position (degenerate direction vector).
 * Event handlers should handle this gracefully 鈥?no crash, no position change.
 */
TEST_F(CameraControllerTest, Edge_CameraAtTarget_NoCrash)
{
	m_camera->SetPosition(glm::vec3(0.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const glm::vec3 initialPos = m_camera->GetPosition();
	const glm::vec3 initialTar = m_camera->cam_tar;

	EXPECT_NO_FATAL_FAILURE(
		m_eventBus.Enqueue(CameraRotateEvent{m_camera->GetObjectID(), 10.0f, 10.0f});
		m_eventBus.Enqueue(CameraZoomEvent{m_camera->GetObjectID(), 5.0f});
		m_eventBus.Process();
	);

	// No position change (degenerate direction 鈥?all ops early-out)
	EXPECT_EQ(m_camera->GetPosition(), initialPos);
	EXPECT_EQ(m_camera->cam_tar, initialTar);
}

// ===========================================================================
// Drag gesture undo bounding (CameraDragBegin/End brackets one undo entry)
// ===========================================================================
//
// Continuous orbit/pan/dolly drags mutate the camera live during the drag
// WITHOUT recording; the whole gesture collapses to one CameraTransformOp
// committed on CameraDragEnd. The fixture's OperationManager has a null scene
// provider, so Undo/Redo replay is a no-op on the camera — but the undo-stack
// bookkeeping (Submit / pop) is scene-independent, so it faithfully proves how
// many entries a gesture records.

/**
 * @test ThreeDragGestures_RecordThreeSeparateEntries
 *
 * Regression for the drag-merge bug: three separate MMB drags on the same
 * camera must produce THREE distinct undo entries, not one. CameraTransformOp
 * is non-mergeable, so consecutive gesture commits never coalesce.
 */
TEST_F(CameraControllerTest, ThreeDragGestures_RecordThreeSeparateEntries)
{
	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	for (int i = 0; i < 3; ++i)
	{
		m_eventBus.Enqueue(CameraDragBegin{m_camera->GetObjectID()});
		m_eventBus.Enqueue(CameraRotateEvent{m_camera->GetObjectID(), 10.0f, 0.0f});
		m_eventBus.Enqueue(CameraRotateEvent{m_camera->GetObjectID(), 0.0f, 5.0f});
		m_eventBus.Enqueue(CameraDragEnd{m_camera->GetObjectID()});
		m_eventBus.Process();
	}

	// Three separate entries: three undos exhaust the stack.
	EXPECT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_FALSE(m_operations.CanUndo());
}

/** @test OnlyCompletedDragsWithMotionCreateHistory */
TEST_F(CameraControllerTest, OnlyCompletedDragsWithMotionCreateHistory)
{
	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	const glm::vec3 posBefore = m_camera->GetPosition();

	m_eventBus.Enqueue(CameraRotateEvent{m_camera->GetObjectID(), 10.0f, 0.0f});
	m_eventBus.Enqueue(CameraSlideEvent{m_camera->GetObjectID(), 10.0f, 0.0f});
	m_eventBus.Enqueue(CameraPushEvent{m_camera->GetObjectID(), 0.0f, 10.0f});
	m_eventBus.Process();

	// Camera moved live...
	EXPECT_NE(m_camera->GetPosition(), posBefore);
	// ...but nothing was recorded.
	EXPECT_FALSE(m_operations.CanUndo());

	m_eventBus.Enqueue(CameraDragBegin{m_camera->GetObjectID()});
	m_eventBus.Enqueue(CameraDragEnd{m_camera->GetObjectID()});
	m_eventBus.Process();

	EXPECT_FALSE(m_operations.CanUndo());
}

/**
 * @test ScrollZoom_DirectionChangeBreaksMerge
 *
 * Same-direction scroll notches coalesce into ONE undo entry, but changing
 * scroll direction starts a NEW entry: zooming in several notches, then
 * zooming out several notches, yields exactly TWO entries (the zoom-in burst
 * merged, the zoom-out burst merged). Undo steps therefore map to direction
 * runs, not the whole scroll session.
 */
TEST_F(CameraControllerTest, ScrollZoom_DirectionChangeBreaksMerge)
{
	m_camera->SetPosition(glm::vec3(0.0f, 5.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	// Three zoom-in notches -> one merged undo entry.
	for (int i = 0; i < 3; ++i)
		m_eventBus.Enqueue(CameraZoomEvent{m_camera->GetObjectID(), 1.0f});
	m_eventBus.Process();

	// One zoom-out notch -> direction changed, so a NEW entry is recorded.
	m_eventBus.Enqueue(CameraZoomEvent{m_camera->GetObjectID(), -1.0f});
	m_eventBus.Process();

	// Another zoom-out notch -> same direction as the previous, merges with it.
	m_eventBus.Enqueue(CameraZoomEvent{m_camera->GetObjectID(), -1.0f});
	m_eventBus.Process();

	// Still exactly two entries: the merged zoom-in run and the merged
	// zoom-out run. Two undos empty the stack.
	m_operations.Undo();
	EXPECT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_FALSE(m_operations.CanUndo());
}
