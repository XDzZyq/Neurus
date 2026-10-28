/**
 * @file test_object_controller.cpp
 * @brief Pooled-object property writes through ObjectController (no GPU).
 *
 * Every fixture object is POOLED but deliberately NOT added to the scene, which
 * is the contract this controller exists for: a property write resolves against
 * the resource pool, so an object is writable before - and whether or not - it
 * becomes scene content (a scene-camera activation candidate, a script-owned
 * object, a detached debug helper). Scene membership and selection stay in
 * test_scene_controller.cpp.
 */

#include <gtest/gtest.h>

#include <memory>

#include "editor/controllers/ObjectController.h"
#include "editor/events/EventBus.h"
#include "editor/events/SceneEvents.h"
#include "editor/events/EditorEvents.h"
#include "editor/operations/OperationManager.h"
#include "core/ResourceManager.h"
#include "core/RenderConfig.h"
#include "scene/Camera.h"
#include "scene/DebugLine.h"
#include "scene/DebugMesh.h"
#include "scene/DebugPoints.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "scene/ObjectID.h"

using namespace neurus;

class ObjectControllerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_controller.Init(m_ctx);

		// Pooled only - none of these enter the scene. The scene provider below
		// fails the test if it is ever queried: resolving a property write through
		// the scene is exactly what this controller must not do.
		m_camera = m_resources.Load<Camera>();
		m_mesh   = m_resources.Load<Mesh>();
		m_light  = m_resources.Load<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
		m_env    = m_resources.Load<Environment>();
		m_line   = m_resources.Load<DebugLine>();
	}

	EventQueue m_eventBus;
	Scene m_scene;
	OperationManager m_operations{ m_eventBus };
	ResourceManager m_resources;
	RenderConfig m_config;
	ControllerContext m_ctx{ m_eventBus, m_resources, m_operations,
	                         [this]() -> Scene* {
		                         ADD_FAILURE() << "ObjectController must not query the scene";
		                         return &m_scene;
	                         },
	                         [this]() { return &m_config; } };
	ObjectController m_controller;
	std::shared_ptr<Camera> m_camera;
	std::shared_ptr<Mesh> m_mesh;
	std::shared_ptr<Light> m_light;
	std::shared_ptr<Environment> m_env;
	std::shared_ptr<DebugLine> m_line;
};

// --- Writes to pooled-but-detached objects ---------------------------------

/**
 * @test The core contract: a property write reaches a pooled object the scene
 *       has never heard of, and is recorded as one undoable edit. Before the
 *       split this event resolved through scene->GetObjectID and was dropped.
 */
TEST_F(ObjectControllerTest, DetachedMeshIsWritable)
{
	m_eventBus.Emit(PositionChanged{ m_mesh->GetObjectID(), 4.0f, 5.0f, 6.0f });

	EXPECT_EQ(m_mesh->GetPosition(), glm::vec3(4.0f, 5.0f, 6.0f));
	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u);
}

TEST_F(ObjectControllerTest, DetachedLightIsWritable)
{
	m_eventBus.Emit(LightPowerChanged{ m_light->GetObjectID(), 42.0f });

	EXPECT_FLOAT_EQ(m_light->light_power, 42.0f);
	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u);
}

TEST_F(ObjectControllerTest, DetachedDebugLineIsWritable)
{
	m_eventBus.Emit(DebugLineWidthChanged{ m_line->GetObjectID(), 7.0f });

	EXPECT_FLOAT_EQ(m_line->GetWidth(), 7.0f);
}

TEST_F(ObjectControllerTest, DetachedEnvironmentIsWritable)
{
	m_eventBus.Emit(EnvironmentIntensityChanged{ m_env->GetObjectID(), 3.5f });

	EXPECT_FLOAT_EQ(m_env->GetIntensity(), 3.5f);
}

TEST_F(ObjectControllerTest, DetachedCameraIsWritable)
{
	m_eventBus.Emit(CameraFovChanged{ m_camera->GetObjectID(), 55.0f });

	EXPECT_FLOAT_EQ(m_camera->cam_pers, 55.0f);
}

// --- Visibility ------------------------------------------------------------

TEST_F(ObjectControllerTest, VisibilityChanged_SetsFlags)
{
	m_eventBus.Emit(VisibilityChanged{ m_mesh->GetObjectID(), false, true });
	EXPECT_FALSE(m_mesh->is_viewport);
	EXPECT_TRUE(m_mesh->is_rendered);
}

TEST_F(ObjectControllerTest, VisibilityChanged_OnLight_EnqueuesLightingRebuild)
{
	bool rebuilt = false;
	m_eventBus.Subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });
	m_eventBus.Emit(VisibilityChanged{ m_light->GetObjectID(), false, true });
	EXPECT_TRUE(rebuilt);
}

// --- Transform -------------------------------------------------------------

TEST_F(ObjectControllerTest, PositionChanged_UpdatesTransform)
{
	m_eventBus.Emit(PositionChanged{ m_mesh->GetObjectID(), 1.0f, 2.0f, 3.0f });
	const glm::vec3& pos = m_mesh->GetPosition();
	EXPECT_FLOAT_EQ(pos.x, 1.0f);
	EXPECT_FLOAT_EQ(pos.y, 2.0f);
	EXPECT_FLOAT_EQ(pos.z, 3.0f);
}

TEST_F(ObjectControllerTest, PositionChanged_OnLight_EnqueuesLightingRebuild)
{
	bool rebuilt = false;
	m_eventBus.Subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });
	m_eventBus.Emit(PositionChanged{ m_light->GetObjectID(), 1.0f, 0.0f, 0.0f });
	EXPECT_TRUE(rebuilt);
}

TEST_F(ObjectControllerTest, ScaleChanged_UpdatesScale)
{
	m_eventBus.Emit(ScaleChanged{ m_mesh->GetObjectID(), 2.0f, 2.0f, 2.0f });
	const glm::vec3& scl = m_mesh->GetScale();
	EXPECT_FLOAT_EQ(scl.x, 2.0f);
	EXPECT_FLOAT_EQ(scl.y, 2.0f);
	EXPECT_FLOAT_EQ(scl.z, 2.0f);
}

// --- Camera ----------------------------------------------------------------

TEST_F(ObjectControllerTest, CameraTargetChanged_Applies)
{
	m_eventBus.Emit(CameraTargetChanged{ m_camera->GetObjectID(), 1.0f, 2.0f, 3.0f });
	EXPECT_EQ(m_camera->cam_tar, glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_F(ObjectControllerTest, CameraFovChanged_Applies)
{
	m_eventBus.Emit(CameraFovChanged{ m_camera->GetObjectID(), 45.0f });
	EXPECT_FLOAT_EQ(m_camera->cam_pers, 45.0f);
}

/**
 * @test The live path of CameraPoseChanged (a script setting Camera.position) is
 *       recorded, not merely applied. The replay path re-enters the same handler
 *       with Submit() suppressed by the phase guard, so one write is one entry.
 */
TEST_F(ObjectControllerTest, CameraPoseChanged_AppliesAndRecordsOneOperation)
{
	const glm::vec3 before = m_camera->GetPosition();
	ASSERT_NE(before, glm::vec3(5.0f, 1.0f, 0.0f)); // a real move, not a no-op write

	m_eventBus.Emit(CameraPoseChanged{ m_camera->GetObjectID(), 5.0f, 1.0f, 0.0f, 2.0f, 0.0f, 0.0f });

	EXPECT_EQ(m_camera->GetPosition(), glm::vec3(5.0f, 1.0f, 0.0f));
	EXPECT_EQ(m_camera->cam_tar, glm::vec3(2.0f, 0.0f, 0.0f));
	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u);
}

// --- Mesh ------------------------------------------------------------------

TEST_F(ObjectControllerTest, MeshShadowChanged_Applies)
{
	m_eventBus.Emit(MeshShadowChanged{ m_mesh->GetObjectID(), false });
	EXPECT_FALSE(m_mesh->using_shadow);
}

TEST_F(ObjectControllerTest, MeshMaterialChanged_Applies)
{
	m_eventBus.Emit(MeshMaterialChanged{ m_mesh->GetObjectID(), false });
	EXPECT_FALSE(m_mesh->using_material);
}

// --- Light -----------------------------------------------------------------

TEST_F(ObjectControllerTest, LightPowerChanged_AppliesAndEnqueuesGpuEvent)
{
	int gpuObjectUid = 0;
	m_eventBus.Subscribe<LightGpuChanged>([&](const LightGpuChanged& e) { gpuObjectUid = e.objectUid; });
	m_eventBus.Emit(LightPowerChanged{ m_light->GetObjectID(), 42.0f });
	EXPECT_FLOAT_EQ(m_light->light_power, 42.0f);
	EXPECT_EQ(gpuObjectUid, m_light->GetObjectID());
}

TEST_F(ObjectControllerTest, LightRadiusChanged_Applies)
{
	m_eventBus.Emit(LightRadiusChanged{ m_light->GetObjectID(), 0.5f });
	EXPECT_FLOAT_EQ(m_light->light_radius, 0.5f);
}

TEST_F(ObjectControllerTest, LightShadowChanged_EnqueuesLightingRebuild)
{
	bool rebuilt = false;
	m_eventBus.Subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });
	m_eventBus.Emit(LightShadowChanged{ m_light->GetObjectID(), false });
	EXPECT_FALSE(m_light->use_shadow);
	EXPECT_TRUE(rebuilt);
}

TEST_F(ObjectControllerTest, LightCutoffChanged_Applies)
{
	m_eventBus.Emit(LightCutoffChanged{ m_light->GetObjectID(), 0.7f });
	EXPECT_FLOAT_EQ(m_light->spot_cutoff, 0.7f);
}

// --- Environment -----------------------------------------------------------

TEST_F(ObjectControllerTest, EnvironmentIntensityChanged_Applies)
{
	m_eventBus.Emit(EnvironmentIntensityChanged{ m_env->GetObjectID(), 2.5f });
	EXPECT_FLOAT_EQ(m_env->GetIntensity(), 2.5f);
}

TEST_F(ObjectControllerTest, EnvironmentRotationChanged_Applies)
{
	m_eventBus.Emit(EnvironmentRotationChanged{ m_env->GetObjectID(), 90.0f });
	EXPECT_FLOAT_EQ(m_env->GetRotation(), 90.0f);
}

// --- Dirty / reset semantics ----------------------------------------------

TEST_F(ObjectControllerTest, PropertyChange_EnqueuesSceneModified)
{
	int modified = 0;
	m_eventBus.Subscribe<SceneModified>([&](const SceneModified&) { modified++; });
	m_eventBus.Emit(CameraFovChanged{ m_camera->GetObjectID(), 45.0f });
	EXPECT_EQ(modified, 1);
}

// --- Scene-scoped GPU upload on demand (SceneObjectGpuUploadRequested) ------

TEST_F(ObjectControllerTest, LightShadowChanged_Enabled_EnqueuesGpuUpload)
{
	int uploadedUid = 0;
	m_eventBus.Subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested& e) {
		uploadedUid = e.objectUid;
	});

	m_eventBus.Emit(LightShadowChanged{ m_light->GetObjectID(), true });

	EXPECT_EQ(uploadedUid, m_light->GetObjectID());
}

TEST_F(ObjectControllerTest, LightShadowChanged_Disabled_DoesNotEnqueueGpuUpload)
{
	bool uploaded = false;
	m_eventBus.Subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested&) {
		uploaded = true;
	});

	m_eventBus.Emit(LightShadowChanged{ m_light->GetObjectID(), false });

	EXPECT_FALSE(uploaded);
}

// --- Debug object properties (issue #22) -----------------------------------

/**
 * @brief ObjectControllerTest plus one pooled object of each debug type.
 *
 * The three types are unrelated types served by one event set, so every shared
 * knob is asserted on all three - that is the part a per-type handler would
 * have gotten wrong.
 */
class DebugPropertyTest : public ObjectControllerTest
{
protected:
	void SetUp() override
	{
		ObjectControllerTest::SetUp();

		m_dline = m_resources.Load<DebugLine>();
		m_dpoints = m_resources.Load<DebugPoints>();
		m_dmesh = m_resources.Load<DebugMesh>();
	}

	std::shared_ptr<DebugLine> m_dline;
	std::shared_ptr<DebugPoints> m_dpoints;
	std::shared_ptr<DebugMesh> m_dmesh;
};

TEST_F(DebugPropertyTest, ColorChanged_AppliesToEveryDebugType)
{
	m_eventBus.Emit(DebugColorChanged{ m_dline->GetObjectID(), 1.0f, 0.0f, 0.0f, 1.0f });
	m_eventBus.Emit(DebugColorChanged{ m_dpoints->GetObjectID(), 0.0f, 1.0f, 0.0f, 0.5f });
	m_eventBus.Emit(DebugColorChanged{ m_dmesh->GetObjectID(), 0.0f, 0.0f, 1.0f, 0.25f });

	EXPECT_EQ(m_dline->GetColor(), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
	EXPECT_EQ(m_dpoints->GetColor(), glm::vec4(0.0f, 1.0f, 0.0f, 0.5f));
	EXPECT_EQ(m_dmesh->GetColor(), glm::vec4(0.0f, 0.0f, 1.0f, 0.25f));
}

TEST_F(DebugPropertyTest, OpacityAndXRayChanged_AppliesToEveryDebugType)
{
	m_eventBus.Emit(DebugOpacityChanged{ m_dline->GetObjectID(), 0.25f });
	m_eventBus.Emit(DebugOpacityChanged{ m_dpoints->GetObjectID(), 0.5f });
	m_eventBus.Emit(DebugOpacityChanged{ m_dmesh->GetObjectID(), 0.75f });
	m_eventBus.Emit(DebugXRayChanged{ m_dline->GetObjectID(), true });
	m_eventBus.Emit(DebugXRayChanged{ m_dpoints->GetObjectID(), true });
	m_eventBus.Emit(DebugXRayChanged{ m_dmesh->GetObjectID(), true });

	EXPECT_FLOAT_EQ(m_dline->GetOpacity(), 0.25f);
	EXPECT_FLOAT_EQ(m_dpoints->GetOpacity(), 0.5f);
	EXPECT_FLOAT_EQ(m_dmesh->GetOpacity(), 0.75f);
	EXPECT_TRUE(m_dline->GetXRay());
	EXPECT_TRUE(m_dpoints->GetXRay());
	EXPECT_TRUE(m_dmesh->GetXRay());
}

TEST_F(DebugPropertyTest, LineKnobsChanged_OnlyReachDebugLine)
{
	m_eventBus.Emit(DebugLineWidthChanged{ m_dline->GetObjectID(), 4.0f });
	m_eventBus.Emit(DebugLineStippleChanged{ m_dline->GetObjectID(), true });
	// Same events aimed at a DebugPoints must be ignored, not mis-applied.
	m_eventBus.Emit(DebugLineWidthChanged{ m_dpoints->GetObjectID(), 9.0f });

	EXPECT_FLOAT_EQ(m_dline->GetWidth(), 4.0f);
	EXPECT_TRUE(m_dline->GetStipple());
}

TEST_F(DebugPropertyTest, PointKnobsChanged_OnlyReachDebugPoints)
{
	m_eventBus.Emit(DebugPointTypeChanged{ m_dpoints->GetObjectID(),
	                                       static_cast<int>(DebugPoints::PointType::CIR) });
	m_eventBus.Emit(DebugPointScaleChanged{ m_dpoints->GetObjectID(), 12.0f });
	m_eventBus.Emit(DebugProjectionModeChanged{ m_dpoints->GetObjectID(), 1 });
	m_eventBus.Emit(DebugPointScaleChanged{ m_dline->GetObjectID(), 99.0f });

	EXPECT_EQ(m_dpoints->GetPointType(), DebugPoints::PointType::CIR);
	EXPECT_FLOAT_EQ(m_dpoints->GetScale(), 12.0f);
	EXPECT_EQ(m_dpoints->GetProjectionMode(), 1);
}

TEST_F(DebugPropertyTest, PositionsChanged_ReplacesTheWholeLineVertexList)
{
	m_dline->PushDebugLine(glm::vec3(0.0f), glm::vec3(1.0f));

	// Three triples: the absolute setter must shrink and grow alike.
	m_eventBus.Emit(DebugPositionsChanged{ m_dline->GetObjectID(),
	                                       { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f } });

	ASSERT_EQ(m_dline->GetVertices().size(), 3u);
	EXPECT_EQ(m_dline->GetVertices()[0], glm::vec3(1.0f, 2.0f, 3.0f));
	EXPECT_EQ(m_dline->GetVertices()[2], glm::vec3(7.0f, 8.0f, 9.0f));
}

TEST_F(DebugPropertyTest, PositionsChanged_ReplacesTheWholePointList)
{
	m_eventBus.Emit(DebugPositionsChanged{ m_dpoints->GetObjectID(),
	                                       { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } });

	ASSERT_EQ(m_dpoints->GetPoints().size(), 2u);
	EXPECT_EQ(m_dpoints->GetPoints()[1], glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_F(DebugPropertyTest, PositionsChanged_DropsATrailingPartialTriple)
{
	// A flattened list is only ever built from whole positions, so a partial tail
	// is malformed input; it is dropped rather than padded with zeros.
	m_eventBus.Emit(DebugPositionsChanged{ m_dpoints->GetObjectID(),
	                                       { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f } });

	ASSERT_EQ(m_dpoints->GetPoints().size(), 1u);
	EXPECT_EQ(m_dpoints->GetPoints()[0], glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_F(DebugPropertyTest, UnknownUid_IsIgnoredAndRecordsNothing)
{
	m_eventBus.Emit(DebugColorChanged{ 999999, 1.0f, 0.0f, 0.0f, 1.0f });
	m_eventBus.Emit(DebugPositionsChanged{ 999999, { 0.0f, 0.0f, 0.0f } });

	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(DebugPropertyTest, ColorChanged_IsUndoableAndRedoable)
{
	const glm::vec4 before = m_dline->GetColor();

	m_eventBus.Emit(DebugColorChanged{ m_dline->GetObjectID(), 0.1f, 0.2f, 0.3f, 0.4f });
	ASSERT_TRUE(m_operations.CanUndo());

	m_operations.Undo();
	EXPECT_EQ(m_dline->GetColor(), before);

	m_operations.Redo();
	EXPECT_EQ(m_dline->GetColor(), glm::vec4(0.1f, 0.2f, 0.3f, 0.4f));
}

TEST_F(DebugPropertyTest, PositionsChanged_IsUndoableAndRedoable)
{
	m_dpoints->PushDebugPoint(glm::vec3(5.0f, 5.0f, 5.0f));

	m_eventBus.Emit(DebugPositionsChanged{ m_dpoints->GetObjectID(),
	                                       { 1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f } });
	ASSERT_EQ(m_dpoints->GetPoints().size(), 2u);

	m_operations.Undo();
	ASSERT_EQ(m_dpoints->GetPoints().size(), 1u);
	EXPECT_EQ(m_dpoints->GetPoints()[0], glm::vec3(5.0f, 5.0f, 5.0f));

	m_operations.Redo();
	ASSERT_EQ(m_dpoints->GetPoints().size(), 2u);
	EXPECT_EQ(m_dpoints->GetPoints()[1], glm::vec3(2.0f, 2.0f, 2.0f));
}

TEST_F(DebugPropertyTest, XRayChanged_UndoRestoresThePreviousFlag)
{
	m_eventBus.Emit(DebugXRayChanged{ m_dmesh->GetObjectID(), true });
	ASSERT_TRUE(m_dmesh->GetXRay());

	m_operations.Undo();
	EXPECT_FALSE(m_dmesh->GetXRay());
}
