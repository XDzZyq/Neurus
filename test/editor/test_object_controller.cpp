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

#include "editor/controllers/ObjectController.hpp"
#include "editor/events/EventBus.hpp"
#include "editor/events/SceneEvents.hpp"
#include "editor/events/EditorEvents.hpp"
#include "editor/operations/OperationManager.hpp"
#include "asset/ResourceManager.hpp"
#include "core/RenderConfig.hpp"
#include "scene/Camera.hpp"
#include "scene/DebugLine.hpp"
#include "scene/DebugMesh.hpp"
#include "scene/DebugPoints.hpp"
#include "scene/Environment.hpp"
#include "scene/Light.hpp"
#include "scene/Mesh.hpp"
#include "scene/Scene.hpp"
#include "scene/ObjectID.hpp"

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

// --- Visibility ------------------------------------------------------------

TEST_F(ObjectControllerTest, LightVisibilityAndPositionInvalidateLighting)
{
	int rebuilds = 0;
	m_eventBus.Subscribe<LightingRebuild>([&](const LightingRebuild&) { ++rebuilds; });
	const int uid = m_light->GetObjectID();
	m_eventBus.Emit(VisibilityChanged{uid, false, true});
	EXPECT_FALSE(m_light->is_viewport);
	EXPECT_TRUE(m_light->is_rendered);
	EXPECT_EQ(rebuilds, 1);
	m_eventBus.Emit(PositionChanged{uid, 1.0f, 0.0f, 0.0f});
	EXPECT_EQ(m_light->GetPosition(), glm::vec3(1.0f, 0.0f, 0.0f));
	EXPECT_EQ(rebuilds, 2);
}

// --- Transform -------------------------------------------------------------

TEST_F(ObjectControllerTest, DetachedMeshAcceptsTransformEdits)
{
	m_eventBus.Emit(PositionChanged{ m_mesh->GetObjectID(), 1.0f, 2.0f, 3.0f });
	const glm::vec3& pos = m_mesh->GetPosition();
	EXPECT_FLOAT_EQ(pos.x, 1.0f);
	EXPECT_FLOAT_EQ(pos.y, 2.0f);
	EXPECT_FLOAT_EQ(pos.z, 3.0f);
	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u);

	m_eventBus.Emit(ScaleChanged{ m_mesh->GetObjectID(), 2.0f, 2.0f, 2.0f });
	const glm::vec3& scl = m_mesh->GetScale();
	EXPECT_FLOAT_EQ(scl.x, 2.0f);
	EXPECT_FLOAT_EQ(scl.y, 2.0f);
	EXPECT_FLOAT_EQ(scl.z, 2.0f);
}

// --- Camera ----------------------------------------------------------------

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
	m_eventBus.Emit(CameraTargetChanged{ m_camera->GetObjectID(), 1.0f, 2.0f, 3.0f });
	EXPECT_EQ(m_camera->cam_tar, glm::vec3(1.0f, 2.0f, 3.0f));
}

// --- Mesh ------------------------------------------------------------------

TEST_F(ObjectControllerTest, DetachedMeshReceivesSurfaceEdits)
{
	m_eventBus.Emit(MeshMaterialChanged{ m_mesh->GetObjectID(), false });
	EXPECT_FALSE(m_mesh->using_material);

	m_eventBus.Emit(MeshShadowChanged{ m_mesh->GetObjectID(), false });
	EXPECT_FALSE(m_mesh->using_shadow);
}

// --- Light -----------------------------------------------------------------

TEST_F(ObjectControllerTest, DetachedLightPropertiesReachThePoolAndInvalidateGpuData)
{
	int gpuObjectUid = 0;
	m_eventBus.Subscribe<LightGpuChanged>([&](const LightGpuChanged& e) { gpuObjectUid = e.objectUid; });
	m_eventBus.Emit(LightPowerChanged{ m_light->GetObjectID(), 42.0f });
	EXPECT_FLOAT_EQ(m_light->light_power, 42.0f);
	EXPECT_EQ(gpuObjectUid, m_light->GetObjectID());
	EXPECT_EQ(m_operations.GetUndoStack().size(), 1u);

	m_eventBus.Emit(LightRadiusChanged{ m_light->GetObjectID(), 0.5f });
	EXPECT_FLOAT_EQ(m_light->light_radius, 0.5f);

	m_eventBus.Emit(LightCutoffChanged{ m_light->GetObjectID(), 0.7f });
	EXPECT_FLOAT_EQ(m_light->spot_cutoff, 0.7f);
}

// --- Environment -----------------------------------------------------------

TEST_F(ObjectControllerTest, DetachedEnvironmentReceivesItsLightingEdits)
{
	m_eventBus.Emit(EnvironmentIntensityChanged{ m_env->GetObjectID(), 2.5f });
	EXPECT_FLOAT_EQ(m_env->GetIntensity(), 2.5f);

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
	EXPECT_FLOAT_EQ(m_camera->cam_pers, 45.0f);
}

// --- Scene-scoped GPU upload on demand (SceneObjectGpuUploadRequested) ------

TEST_F(ObjectControllerTest, ShadowChangesRebuildLightingAndOnlyEnablingUploads)
{
	int uploadedUid = 0;
	bool rebuilt = false;
	m_eventBus.Subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });
	m_eventBus.Subscribe<SceneObjectGpuUploadRequested>([&](const auto& event) { uploadedUid = event.objectUid; });
	const int uid = m_light->GetObjectID();
	for (const bool enabled : { false, true })
	{
		SCOPED_TRACE(enabled);
		rebuilt = false;
		uploadedUid = 0;
		m_eventBus.Emit(LightShadowChanged{uid, enabled});
		EXPECT_EQ(m_light->use_shadow, enabled);
		EXPECT_TRUE(rebuilt);
		EXPECT_EQ(uploadedUid, enabled ? uid : 0);
	}
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

TEST_F(DebugPropertyTest, DebugAppearanceEditsApplyAndUndoForEveryShape)
{
	auto checkAppearance = [&](const auto& shape, const char* name, glm::vec4 color, float opacity)
	{
		SCOPED_TRACE(name);
		const int uid = shape->GetObjectID();
		const auto previousColor = shape->GetColor();
		const bool previousXray = shape->GetXRay();
		m_eventBus.Emit(DebugColorChanged{uid, color.r, color.g, color.b, color.a});
		EXPECT_EQ(shape->GetColor(), color);
		m_operations.Undo();
		EXPECT_EQ(shape->GetColor(), previousColor);
		m_operations.Redo();
		EXPECT_EQ(shape->GetColor(), color);
		m_eventBus.Emit(DebugOpacityChanged{uid, opacity});
		EXPECT_FLOAT_EQ(shape->GetOpacity(), opacity);
		m_eventBus.Emit(DebugXRayChanged{uid, !previousXray});
		EXPECT_EQ(shape->GetXRay(), !previousXray);
		m_operations.Undo();
		EXPECT_EQ(shape->GetXRay(), previousXray);
	};
	checkAppearance(m_dline, "line", {1.0f, 0.0f, 0.0f, 1.0f}, 0.25f);
	checkAppearance(m_dpoints, "points", {0.0f, 1.0f, 0.0f, 0.5f}, 0.5f);
	checkAppearance(m_dmesh, "mesh", {0.0f, 0.0f, 1.0f, 0.25f}, 0.75f);
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

TEST_F(DebugPropertyTest, UnknownUid_IsIgnoredAndRecordsNothing)
{
	m_eventBus.Emit(DebugColorChanged{ 999999, 1.0f, 0.0f, 0.0f, 1.0f });
	m_eventBus.Emit(DebugPositionsChanged{ 999999, { 0.0f, 0.0f, 0.0f } });

	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(DebugPropertyTest, PositionsChanged_IsUndoableAndRedoable)
{
	m_dpoints->PushDebugPoint(glm::vec3(5.0f, 5.0f, 5.0f));

	m_eventBus.Emit(DebugPositionsChanged{ m_dpoints->GetObjectID(),
	                                       { 1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f, 3.0f, 4.0f } });
	ASSERT_EQ(m_dpoints->GetPoints().size(), 2u);

	EXPECT_EQ(m_dpoints->GetPoints()[0], glm::vec3(1.0f));
	EXPECT_EQ(m_dpoints->GetPoints()[1], glm::vec3(2.0f));
	m_operations.Undo();
	ASSERT_EQ(m_dpoints->GetPoints().size(), 1u);
	EXPECT_EQ(m_dpoints->GetPoints()[0], glm::vec3(5.0f, 5.0f, 5.0f));

	m_operations.Redo();
	ASSERT_EQ(m_dpoints->GetPoints().size(), 2u);
	EXPECT_EQ(m_dpoints->GetPoints()[1], glm::vec3(2.0f, 2.0f, 2.0f));
}
