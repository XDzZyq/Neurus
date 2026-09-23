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
 * Sensitivity constants (from CameraController.h):
 *   - kOrbitSensitivity = 0.005f  (radians per pixel)
 *   - kPanSensitivity   = 0.01f   (world units per pixel)
 *   - kZoomSensitivity  = 1.0f    (factor per scroll notch)
 *   - kDollySensitivity = 0.05f   (world units per pixel)
 */

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "editor/controllers/CameraController.h"
#include "editor/events/CameraEvents.h"
#include "editor/operations/OperationManager.h"
#include "core/ResourceManager.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "render/RenderConfig.h"

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

/**
 * @test Orbit_MMB_Right_Drag_AzimuthIncreases
 *
 * Camera at (5,0,0), target at origin. MMB drag right (positive mouseDeltaX)
 * should rotate azimuth around world Z axis, producing a non-zero Y component
 * (forward axis in Z-up convention). Target remains unchanged.
 */
TEST_F(CameraControllerTest, Orbit_MMB_Right_Drag_AzimuthIncreases)
{
	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const glm::vec3 initialTar = m_camera->cam_tar;

	CameraRotateEvent e{m_camera->GetObjectID(), 10.0f, 0.0f};
	m_eventBus.enqueue(e);
	m_eventBus.Process();

	// Camera Y should be non-zero (azimuth rotated right around target, forward axis)
	EXPECT_NE(m_camera->GetPosition().y, 0.0f);
	// Target unchanged
	EXPECT_EQ(m_camera->cam_tar, initialTar);
}

/**
 * @test Orbit_MMB_Up_Drag_ElevationIncreases
 *
 * Camera at (5,0,0), target at origin. MMB drag up (positive mouseDeltaY)
 * should decrease elevation (drag up = look down, natural 3D convention).
 * Camera Z position decreases (elevation axis in Z-up). Target remains unchanged.
 */
TEST_F(CameraControllerTest, Orbit_MMB_Up_Drag_ElevationIncreases)
{
	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const float initialZ = m_camera->GetPosition().z;
	const glm::vec3 initialTar = m_camera->cam_tar;

	CameraRotateEvent e{m_camera->GetObjectID(), 0.0f, -10.0f}; // deltaY negative = drag up = look down
	m_eventBus.enqueue(e);
	m_eventBus.Process();

	// Elevation decreased (drag up = look down) camera Z should be lower
	EXPECT_LT(m_camera->GetPosition().z, initialZ);
	// Target unchanged
	EXPECT_EQ(m_camera->cam_tar, initialTar);
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
		m_eventBus.enqueue(e);
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

/**
 * @test Zoom_ScrollUp_Decreases_Distance
 *
 * Camera at (0,5,0), target at origin (Z-up: +Y is forward).
 * Scroll up (positive scroll_dir) should zoom in, decreasing the
 * distance from camera to target.
 */
TEST_F(CameraControllerTest, Zoom_ScrollUp_Decreases_Distance)
{
	m_camera->SetPosition(glm::vec3(0.0f, 5.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const float initialDist = DistanceToTarget(*m_camera);
	const glm::vec3 initialTar = m_camera->cam_tar;

	CameraZoomEvent e{m_camera->GetObjectID(), 1.0f};
	m_eventBus.enqueue(e);
	m_eventBus.Process();

	EXPECT_LT(DistanceToTarget(*m_camera), initialDist);
	EXPECT_EQ(m_camera->cam_tar, initialTar);
}

/**
 * @test Zoom_ScrollDown_Increases_Distance
 *
 * Camera at (0,5,0), target at origin (Z-up: +Y is forward).
 * Scroll down (negative scroll_dir) should zoom out, increasing the
 * distance from camera to target.
 */
TEST_F(CameraControllerTest, Zoom_ScrollDown_Increases_Distance)
{
	m_camera->SetPosition(glm::vec3(0.0f, 5.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const float initialDist = DistanceToTarget(*m_camera);
	const glm::vec3 initialTar = m_camera->cam_tar;

	CameraZoomEvent e{m_camera->GetObjectID(), -1.0f};
	m_eventBus.enqueue(e);
	m_eventBus.Process();

	EXPECT_GT(DistanceToTarget(*m_camera), initialDist);
	EXPECT_EQ(m_camera->cam_tar, initialTar);
}

/**
 * @test Zoom_Clamp_MinRadius
 *
 * Camera already at minimum distance (0.01) from target along forward axis
 * (Z-up: +Y is forward). Attempting to zoom in further should not reduce
 * distance below the minimum clamp. No crash.
 */
TEST_F(CameraControllerTest, Zoom_Clamp_MinRadius)
{
	m_camera->SetPosition(glm::vec3(0.0f, 0.01f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	CameraZoomEvent e{m_camera->GetObjectID(), 1.0f}; // Try to zoom in more

	EXPECT_NO_FATAL_FAILURE(
		m_eventBus.enqueue(e);
		m_eventBus.Process();
	);

	// Distance must stay at or above the minimum clamp
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
	m_eventBus.enqueue(e);
	m_eventBus.Process();

	// Camera moved along forward axis (Y changed)
	EXPECT_NE(m_camera->GetPosition().y, initialY);
	// X unchanged (movement is strictly along view direction)
	EXPECT_FLOAT_EQ(m_camera->GetPosition().x, initialX);
	// Target stays fixed
	EXPECT_EQ(m_camera->cam_tar, initialTar);
}

/**
 * @test Dolly_WithoutMMB_NoMovement
 *
 * No event enqueued — camera should not change.
 */
TEST_F(CameraControllerTest, Dolly_WithoutMMB_NoMovement)
{
	m_camera->SetPosition(glm::vec3(0.0f, 5.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const glm::vec3 initialPos = m_camera->GetPosition();
	const glm::vec3 initialTar = m_camera->cam_tar;

	// No event — camera unchanged
	m_eventBus.Process();

	EXPECT_EQ(m_camera->GetPosition(), initialPos);
	EXPECT_EQ(m_camera->cam_tar, initialTar);
}

// ===========================================================================
// Pan tests (Shift + MMB 鈫?CameraSlideEvent)
// ===========================================================================

/**
 * @test Pan_ShiftMMB_Right_MovesCameraAndTarget
 *
 * Camera at (5,0,0), target at origin. Shift+MMB drag right
 * (positive mouseDeltaX) should pan both camera and target along
 * the camera's right vector. The offset between them is preserved.
 */
TEST_F(CameraControllerTest, Pan_ShiftMMB_Right_MovesCameraAndTarget)
{
	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const glm::vec3 initialPos = m_camera->GetPosition();
	const glm::vec3 initialTar = m_camera->cam_tar;

	CameraSlideEvent e{m_camera->GetObjectID(), 10.0f, 0.0f};
	m_eventBus.enqueue(e);
	m_eventBus.Process();

	// Both camera position and target changed (pan shifts both)
	EXPECT_NE(m_camera->GetPosition(), initialPos);
	EXPECT_NE(m_camera->cam_tar, initialTar);

	// Position-target offset preserved (pure translation, no rotation)
	const glm::vec3 offsetBefore = initialPos - initialTar;
	const glm::vec3 offsetAfter  = m_camera->GetPosition() - m_camera->cam_tar;
	EXPECT_NEAR(offsetAfter.x, offsetBefore.x, 1e-4f);
	EXPECT_NEAR(offsetAfter.y, offsetBefore.y, 1e-4f);
	EXPECT_NEAR(offsetAfter.z, offsetBefore.z, 1e-4f);
}

/**
 * @test Pan_ShiftMMB_Up_MovesUp
 *
 * Camera at (5,0,0), target at origin. Shift+MMB drag up
 * (positive mouseDeltaY) should pan vertically, changing Z of
 * both camera and target (Z is up in Z-up convention).
 */
TEST_F(CameraControllerTest, Pan_ShiftMMB_Up_MovesUp)
{
	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const float initialZ = m_camera->GetPosition().z;
	const float initialTarZ = m_camera->cam_tar.z;

	CameraSlideEvent e{m_camera->GetObjectID(), 0.0f, 10.0f};
	m_eventBus.enqueue(e);
	m_eventBus.Process();

	// Vertical pan should change Z of both camera and target (Z-up)
	EXPECT_NE(m_camera->GetPosition().z, initialZ);
	EXPECT_NE(m_camera->cam_tar.z, initialTarZ);
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
		m_eventBus.enqueue(CameraRotateEvent{m_camera->GetObjectID(), 10.0f, 10.0f});
		m_eventBus.enqueue(CameraZoomEvent{m_camera->GetObjectID(), 5.0f});
		m_eventBus.Process();
	);

	// No position change (degenerate direction 鈥?all ops early-out)
	EXPECT_EQ(m_camera->GetPosition(), initialPos);
	EXPECT_EQ(m_camera->cam_tar, initialTar);
}

/**
 * @test Edge_NoInput_NoChange
 *
 * No events enqueued — camera should not change.
 */
TEST_F(CameraControllerTest, Edge_NoInput_NoChange)
{
	m_camera->SetPosition(glm::vec3(3.0f, 5.0f, 4.0f));
	m_camera->SetTarPos(glm::vec3(1.0f, 3.0f, 2.0f));
	const glm::vec3 initialPos = m_camera->GetPosition();
	const glm::vec3 initialTar = m_camera->cam_tar;

	// No events enqueued — camera unchanged
	m_eventBus.Process();

	EXPECT_EQ(m_camera->GetPosition(), initialPos);
	EXPECT_EQ(m_camera->cam_tar, initialTar);
}

/**
 * @test Edge_ModifierConflict_CtrlWins
 *
 * When both Ctrl and Shift are held (event system decides), the appropriate
 * event type (CameraPushEvent for Ctrl priority) should be enqueued.
 * Target should NOT move (dolly behavior, not pan).
 */
TEST_F(CameraControllerTest, Edge_ModifierConflict_CtrlWins)
{
	m_camera->SetPosition(glm::vec3(0.0f, 5.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const glm::vec3 initialTar = m_camera->cam_tar;

	// Ctrl wins → CameraPushEvent (dolly), not CameraSlideEvent (pan)
	CameraPushEvent e{m_camera->GetObjectID(), 0.0f, 10.0f};
	m_eventBus.enqueue(e);
	m_eventBus.Process();

	// Ctrl wins → Dolly behavior: target should NOT move
	EXPECT_EQ(m_camera->cam_tar, initialTar);
	// Camera should have moved (dolly along forward, Y in Z-up)
	EXPECT_NE(m_camera->GetPosition().y, 5.0f);
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
 * @test DragGesture_RecordsSingleUndoEntry
 *
 * Begin → two orbit moves → End should record exactly one undo entry, no
 * matter how many intermediate moves the drag contained.
 */
TEST_F(CameraControllerTest, DragGesture_RecordsSingleUndoEntry)
{
	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	m_eventBus.enqueue(CameraDragBegin{m_camera->GetObjectID()});
	m_eventBus.enqueue(CameraRotateEvent{m_camera->GetObjectID(), 10.0f, 0.0f});
	m_eventBus.enqueue(CameraRotateEvent{m_camera->GetObjectID(), 10.0f, 0.0f});
	m_eventBus.enqueue(CameraDragEnd{m_camera->GetObjectID()});
	m_eventBus.Process();

	ASSERT_TRUE(m_operations.CanUndo());

	// Exactly one entry: a single undo empties the stack (moves it to redo).
	m_operations.Undo();
	EXPECT_FALSE(m_operations.CanUndo());
	EXPECT_TRUE(m_operations.CanRedo());
}

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
		m_eventBus.enqueue(CameraDragBegin{m_camera->GetObjectID()});
		m_eventBus.enqueue(CameraRotateEvent{m_camera->GetObjectID(), 10.0f, 0.0f});
		m_eventBus.enqueue(CameraDragEnd{m_camera->GetObjectID()});
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

/**
 * @test DragMoves_WithoutGesture_RecordNothing
 *
 * Orbit moves that arrive outside a begin/end bracket mutate the camera live
 * but must NOT record — otherwise every intermediate drag frame would pollute
 * the history.
 */
TEST_F(CameraControllerTest, DragMoves_WithoutGesture_RecordNothing)
{
	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	const glm::vec3 posBefore = m_camera->GetPosition();

	m_eventBus.enqueue(CameraRotateEvent{m_camera->GetObjectID(), 10.0f, 0.0f});
	m_eventBus.enqueue(CameraSlideEvent{m_camera->GetObjectID(), 10.0f, 0.0f});
	m_eventBus.enqueue(CameraPushEvent{m_camera->GetObjectID(), 0.0f, 10.0f});
	m_eventBus.Process();

	// Camera moved live...
	EXPECT_NE(m_camera->GetPosition(), posBefore);
	// ...but nothing was recorded.
	EXPECT_FALSE(m_operations.CanUndo());
}

/**
 * @test DragGesture_NoMovement_RecordsNothing
 *
 * A begin/end bracket with no intervening move (e.g. a click that didn't drag)
 * leaves the pose unchanged, so no undo entry is recorded.
 */
TEST_F(CameraControllerTest, DragGesture_NoMovement_RecordsNothing)
{
	m_camera->SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	m_eventBus.enqueue(CameraDragBegin{m_camera->GetObjectID()});
	m_eventBus.enqueue(CameraDragEnd{m_camera->GetObjectID()});
	m_eventBus.Process();

	EXPECT_FALSE(m_operations.CanUndo());
}

/**
 * @test ScrollZoom_RecordsPerEvent_WithoutGesture
 *
 * Scroll zoom has no press/release, so it keeps recording per-event as a
 * CameraZoomOp (coalesced by MergeKey at the OperationManager level). A single
 * zoom records one entry even with no drag gesture around it.
 */
TEST_F(CameraControllerTest, ScrollZoom_RecordsPerEvent_WithoutGesture)
{
	m_camera->SetPosition(glm::vec3(0.0f, 5.0f, 0.0f));
	m_camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	m_eventBus.enqueue(CameraZoomEvent{m_camera->GetObjectID(), 1.0f});
	m_eventBus.Process();

	EXPECT_TRUE(m_operations.CanUndo());
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
		m_eventBus.enqueue(CameraZoomEvent{m_camera->GetObjectID(), 1.0f});
	m_eventBus.Process();

	// One zoom-out notch -> direction changed, so a NEW entry is recorded.
	m_eventBus.enqueue(CameraZoomEvent{m_camera->GetObjectID(), -1.0f});
	m_eventBus.Process();

	// Another zoom-out notch -> same direction as the previous, merges with it.
	m_eventBus.enqueue(CameraZoomEvent{m_camera->GetObjectID(), -1.0f});
	m_eventBus.Process();

	// Still exactly two entries: the merged zoom-in run and the merged
	// zoom-out run. Two undos empty the stack.
	m_operations.Undo();
	EXPECT_TRUE(m_operations.CanUndo());
	m_operations.Undo();
	EXPECT_FALSE(m_operations.CanUndo());
}
