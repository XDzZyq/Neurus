#include <gtest/gtest.h>

#include <memory>

#include "editor/controllers/SceneController.h"
#include "editor/events/EventBus.h"
#include "editor/events/SceneEvents.h"
#include "editor/events/EditorEvents.h"
#include "editor/operations/OperationManager.h"
#include "editor/Input.h"
#include "core/ResourceManager.h"
#include "asset/data/ImageData.h"
#include "asset/data/MeshData.h"
#include "scene/Camera.h"
#include "scene/DebugLine.h"
#include "scene/DebugMesh.h"
#include "scene/DebugPoints.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "scene/ObjectID.h"
#include "render/RenderConfig.h"

using namespace neurus;

class SceneControllerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_controller.Init(m_ctx);

		m_camera = std::make_shared<Camera>();
		m_mesh   = std::make_shared<Mesh>();
		m_light  = std::make_shared<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
		m_env    = std::make_shared<Environment>();
		m_scene.UseCamera(m_camera);
		m_scene.UseMesh(m_mesh);
		m_scene.UseLight(m_light);
		m_scene.UseEnvironment(m_env);
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
	std::shared_ptr<Camera> m_camera;
	std::shared_ptr<Mesh> m_mesh;
	std::shared_ptr<Light> m_light;
	std::shared_ptr<Environment> m_env;
};

// --- Selection -------------------------------------------------------------

TEST_F(SceneControllerTest, ObjectSelected_SelectsObject)
{
	m_eventBus.enqueue(ObjectSelected{m_mesh->GetObjectID(), 0});
	Process();
	EXPECT_TRUE(m_scene.selections.IsSelected(m_mesh.get()));
	EXPECT_EQ(m_scene.selections.GetActiveObject(), m_mesh.get());
}

TEST_F(SceneControllerTest, ObjectSelected_NullObject_ClearsSelection)
{
	m_scene.selections.Select(m_mesh.get(), false);
	m_eventBus.enqueue(ObjectSelected{0, 0});
	Process();
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 0);
}

TEST_F(SceneControllerTest, ObjectSelected_Shift_AddsToSelection)
{
	m_eventBus.enqueue(ObjectSelected{m_mesh->GetObjectID(), 0});
	m_eventBus.enqueue(ObjectSelected{m_light->GetObjectID(),
	                                  static_cast<int>(Input::Mod_Shift)});
	Process();
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 2);
}

TEST_F(SceneControllerTest, ObjectDeselected_RemovesFromSelection)
{
	m_scene.selections.Select(m_mesh.get(), false);
	m_eventBus.enqueue(ObjectDeselected{m_mesh->GetObjectID()});
	Process();
	EXPECT_FALSE(m_scene.selections.IsSelected(m_mesh.get()));
}

// --- Visibility ------------------------------------------------------------

TEST_F(SceneControllerTest, VisibilityChanged_SetsFlags)
{
	m_eventBus.enqueue(VisibilityChanged{m_mesh->GetObjectID(), false, true});
	Process();
	EXPECT_FALSE(m_mesh->is_viewport);
	EXPECT_TRUE(m_mesh->is_rendered);
}

TEST_F(SceneControllerTest, VisibilityChanged_OnLight_EnqueuesLightingRebuild)
{
	bool rebuilt = false;
	m_eventBus.subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });
	m_eventBus.enqueue(VisibilityChanged{m_light->GetObjectID(), false, true});
	Process();
	EXPECT_TRUE(rebuilt);
}

// --- Transform -------------------------------------------------------------

TEST_F(SceneControllerTest, PositionChanged_UpdatesTransform)
{
	m_eventBus.enqueue(PositionChanged{m_mesh->GetObjectID(), 1.0f, 2.0f, 3.0f});
	Process();
	const glm::vec3& pos = m_mesh->GetPosition();
	EXPECT_FLOAT_EQ(pos.x, 1.0f);
	EXPECT_FLOAT_EQ(pos.y, 2.0f);
	EXPECT_FLOAT_EQ(pos.z, 3.0f);
}

TEST_F(SceneControllerTest, PositionChanged_OnLight_EnqueuesLightingRebuild)
{
	bool rebuilt = false;
	m_eventBus.subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });
	m_eventBus.enqueue(PositionChanged{m_light->GetObjectID(), 1.0f, 0.0f, 0.0f});
	Process();
	EXPECT_TRUE(rebuilt);
}

TEST_F(SceneControllerTest, ScaleChanged_UpdatesScale)
{
	m_eventBus.enqueue(ScaleChanged{m_mesh->GetObjectID(), 2.0f, 2.0f, 2.0f});
	Process();
	const glm::vec3& scl = m_mesh->GetScale();
	EXPECT_FLOAT_EQ(scl.x, 2.0f);
	EXPECT_FLOAT_EQ(scl.y, 2.0f);
	EXPECT_FLOAT_EQ(scl.z, 2.0f);
}

// --- Camera ----------------------------------------------------------------

TEST_F(SceneControllerTest, CameraTargetChanged_Applies)
{
	m_eventBus.enqueue(CameraTargetChanged{m_camera->GetObjectID(), 1.0f, 2.0f, 3.0f});
	Process();
	EXPECT_EQ(m_camera->cam_tar, glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_F(SceneControllerTest, CameraFovChanged_Applies)
{
	m_eventBus.enqueue(CameraFovChanged{m_camera->GetObjectID(), 45.0f});
	Process();
	EXPECT_FLOAT_EQ(m_camera->cam_pers, 45.0f);
}

// --- Mesh ------------------------------------------------------------------

TEST_F(SceneControllerTest, MeshShadowChanged_Applies)
{
	m_eventBus.enqueue(MeshShadowChanged{m_mesh->GetObjectID(), false});
	Process();
	EXPECT_FALSE(m_mesh->using_shadow);
}

TEST_F(SceneControllerTest, MeshMaterialChanged_Applies)
{
	m_eventBus.enqueue(MeshMaterialChanged{m_mesh->GetObjectID(), false});
	Process();
	EXPECT_FALSE(m_mesh->using_material);
}

// --- Light -----------------------------------------------------------------

TEST_F(SceneControllerTest, LightPowerChanged_AppliesAndEnqueuesGpuEvent)
{
	int gpuObjectUid = 0;
	m_eventBus.subscribe<LightGpuChanged>([&](const LightGpuChanged& e) { gpuObjectUid = e.objectUid; });
	m_eventBus.enqueue(LightPowerChanged{m_light->GetObjectID(), 42.0f});
	Process();
	EXPECT_FLOAT_EQ(m_light->light_power, 42.0f);
	EXPECT_EQ(gpuObjectUid, m_light->GetObjectID());
}

TEST_F(SceneControllerTest, LightRadiusChanged_Applies)
{
	m_eventBus.enqueue(LightRadiusChanged{m_light->GetObjectID(), 0.5f});
	Process();
	EXPECT_FLOAT_EQ(m_light->light_radius, 0.5f);
}

TEST_F(SceneControllerTest, LightShadowChanged_EnqueuesLightingRebuild)
{
	bool rebuilt = false;
	m_eventBus.subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });
	m_eventBus.enqueue(LightShadowChanged{m_light->GetObjectID(), false});
	Process();
	EXPECT_FALSE(m_light->use_shadow);
	EXPECT_TRUE(rebuilt);
}

TEST_F(SceneControllerTest, LightCutoffChanged_Applies)
{
	m_eventBus.enqueue(LightCutoffChanged{m_light->GetObjectID(), 0.7f});
	Process();
	EXPECT_FLOAT_EQ(m_light->spot_cutoff, 0.7f);
}

// --- Environment -----------------------------------------------------------

TEST_F(SceneControllerTest, EnvironmentIntensityChanged_Applies)
{
	m_eventBus.enqueue(EnvironmentIntensityChanged{m_env->GetObjectID(), 2.5f});
	Process();
	EXPECT_FLOAT_EQ(m_env->GetIntensity(), 2.5f);
}

TEST_F(SceneControllerTest, EnvironmentRotationChanged_Applies)
{
	m_eventBus.enqueue(EnvironmentRotationChanged{m_env->GetObjectID(), 90.0f});
	Process();
	EXPECT_FLOAT_EQ(m_env->GetRotation(), 90.0f);
}

// --- Dirty / reset semantics ----------------------------------------------

TEST_F(SceneControllerTest, PropertyChange_EnqueuesSceneModified)
{
	int modified = 0;
	m_eventBus.subscribe<SceneModified>([&](const SceneModified&) { modified++; });
	m_eventBus.enqueue(CameraFovChanged{m_camera->GetObjectID(), 45.0f});
	Process();
	EXPECT_EQ(modified, 1);
}

TEST_F(SceneControllerTest, Selection_DoesNotEnqueueSceneModified)
{
	int modified = 0;
	m_eventBus.subscribe<SceneModified>([&](const SceneModified&) { modified++; });
	m_eventBus.enqueue(ObjectSelected{m_mesh->GetObjectID(), 0});
	Process();
	EXPECT_EQ(modified, 0);
}

// --- Scene membership (Add / Delete) --------------------------------------

/** @brief Loads a mesh into the pool and returns its UID. */
int LoadPooledMesh(ResourceManager& resources)
{
	auto meshData = resources.Load<MeshData>();
	auto mesh = resources.Load<Mesh>(meshData);
	return mesh->GetObjectID();
}

TEST_F(SceneControllerTest, SceneObjectAdd_RegistersSelectsAndRecordsOp)
{
	const int uid = LoadPooledMesh(m_resources);

	m_eventBus.enqueue(SceneObjectAddRequested{uid});
	Process();

	EXPECT_EQ(m_scene.mesh_list.count(uid), 1u);
	EXPECT_NE(m_scene.GetObjectID(uid), nullptr);
	EXPECT_TRUE(m_scene.selections.IsSelected(m_scene.GetObjectID(uid)));
	EXPECT_EQ(m_scene.selections.GetActiveObject(), m_scene.GetObjectID(uid));
	EXPECT_TRUE(m_operations.CanUndo());

	// Undo: object removed, selection restored to the pre-add (empty) set.
	m_operations.Undo();
	EXPECT_EQ(m_scene.mesh_list.count(uid), 0u);
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 0u);

	// Redo: object re-registered (from the pool — no reload) + re-selected.
	m_operations.Redo();
	EXPECT_EQ(m_scene.mesh_list.count(uid), 1u);
	EXPECT_TRUE(m_scene.selections.IsSelected(m_scene.GetObjectID(uid)));
}

TEST_F(SceneControllerTest, SceneObjectAdd_AlreadyInScene_NoOp)
{
	const int uid = LoadPooledMesh(m_resources);
	m_eventBus.enqueue(SceneObjectAddRequested{uid});
	Process();

	m_eventBus.enqueue(SceneObjectAddRequested{uid});
	Process();
	EXPECT_EQ(m_scene.mesh_list.count(uid), 1u);
}

TEST_F(SceneControllerTest, SceneObjectAdd_StaleUid_NoOp)
{
	const int uid = LoadPooledMesh(m_resources);
	m_resources.Remove(uid); // resource no longer pooled

	m_eventBus.enqueue(SceneObjectAddRequested{uid});
	Process();
	EXPECT_EQ(m_scene.mesh_list.count(uid), 0u);
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(SceneControllerTest, DeleteRequested_RemovesSelectionAndRecordsComposite)
{
	const int uid = LoadPooledMesh(m_resources);
	m_eventBus.enqueue(SceneObjectAddRequested{uid});
	Process(); // mesh added + selected

	m_eventBus.enqueue(ObjectDeleteRequested{});
	Process();

	EXPECT_EQ(m_scene.mesh_list.count(uid), 0u);
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 0u);
	EXPECT_TRUE(m_operations.CanUndo());

	// Undo: mesh re-added (from the pool) + selection restored.
	m_operations.Undo();
	EXPECT_EQ(m_scene.mesh_list.count(uid), 1u);
	EXPECT_TRUE(m_scene.selections.IsSelected(m_scene.GetObjectID(uid)));

	// Redo: deleted again.
	m_operations.Redo();
	EXPECT_EQ(m_scene.mesh_list.count(uid), 0u);
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 0u);
}

TEST_F(SceneControllerTest, DeleteRequested_MultiSelection_RemovesAllAndRestores)
{
	const int uidA = LoadPooledMesh(m_resources);
	const int uidB = LoadPooledMesh(m_resources);
	m_eventBus.enqueue(SceneObjectAddRequested{uidA});
	m_eventBus.enqueue(SceneObjectAddRequested{uidB});
	Process(); // both added; B selected last

	// Multi-select both (shift-add semantics).
	m_scene.selections.Select(m_scene.GetObjectID(uidA), true);

	m_eventBus.enqueue(ObjectDeleteRequested{});
	Process();

	EXPECT_EQ(m_scene.mesh_list.count(uidA), 0u);
	EXPECT_EQ(m_scene.mesh_list.count(uidB), 0u);
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 0u);

	m_operations.Undo();
	EXPECT_EQ(m_scene.mesh_list.count(uidA), 1u);
	EXPECT_EQ(m_scene.mesh_list.count(uidB), 1u);
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 2u);

	m_operations.Redo();
	// Only the fixture mesh (registered in SetUp) remains.
	EXPECT_EQ(m_scene.mesh_list.size(), 1u);
	EXPECT_EQ(m_scene.mesh_list.count(uidA), 0u);
	EXPECT_EQ(m_scene.mesh_list.count(uidB), 0u);
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 0u);
}

TEST_F(SceneControllerTest, DeleteRequested_LastCamera_Refused)
{
	m_scene.selections.Select(m_camera.get(), false);
	m_eventBus.enqueue(ObjectDeleteRequested{});
	Process();

	EXPECT_EQ(m_scene.cam_list.count(m_camera->GetObjectID()), 1u);
	EXPECT_FALSE(m_operations.CanUndo()); // nothing recorded
}

TEST_F(SceneControllerTest, DeleteRequested_EmptySelection_NoOp)
{
	m_eventBus.enqueue(ObjectDeleteRequested{});
	Process();
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(SceneControllerTest, SceneObjectAdd_Light_EnqueuesLightingRebuild)
{
	bool rebuilt = false;
	m_eventBus.subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });

	auto light = m_resources.Load<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
	m_eventBus.enqueue(SceneObjectAddRequested{light->GetObjectID()});
	Process();
	EXPECT_TRUE(rebuilt);
	EXPECT_EQ(m_scene.light_list.count(light->GetObjectID()), 1u);
}

TEST_F(SceneControllerTest, DeleteRequested_Light_EnqueuesLightingRebuild)
{
	auto light = m_resources.Load<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
	m_eventBus.enqueue(SceneObjectAddRequested{light->GetObjectID()});
	Process();

	bool rebuilt = false;
	m_eventBus.subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });
	m_eventBus.enqueue(ObjectDeleteRequested{});
	Process();

	EXPECT_TRUE(rebuilt);
	EXPECT_EQ(m_scene.light_list.count(light->GetObjectID()), 0u);
}

// --- Scene-scoped GPU upload on demand (SceneObjectGpuUploadRequested) ------

TEST_F(SceneControllerTest, SceneObjectAdd_Mesh_EnqueuesGpuUpload)
{
	int uploadedUid = 0;
	m_eventBus.subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested& e) {
		uploadedUid = e.objectUid;
	});

	auto mesh = m_resources.Load<Mesh>(m_resources.Load<MeshData>());
	m_eventBus.enqueue(SceneObjectAddRequested{mesh->GetObjectID()});
	Process();

	EXPECT_EQ(uploadedUid, mesh->GetObjectID()); // the re-added mesh
	EXPECT_EQ(m_scene.mesh_list.count(mesh->GetObjectID()), 1u);
}

TEST_F(SceneControllerTest, SceneObjectAdd_Light_EnqueuesGpuUpload)
{
	int uploadedUid = 0;
	m_eventBus.subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested& e) {
		uploadedUid = e.objectUid;
	});

	auto light = m_resources.Load<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
	m_eventBus.enqueue(SceneObjectAddRequested{light->GetObjectID()});
	Process();

	EXPECT_EQ(uploadedUid, light->GetObjectID());
	EXPECT_EQ(m_scene.light_list.count(light->GetObjectID()), 1u);
}

TEST_F(SceneControllerTest, SceneObjectAdd_Environment_EnqueuesGpuUpload)
{
	int uploadedUid = 0;
	m_eventBus.subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested& e) {
		uploadedUid = e.objectUid;
	});

	auto env = m_resources.Load<Environment>(m_resources.Load<ImageData>(""));
	m_eventBus.enqueue(SceneObjectAddRequested{env->GetObjectID()});
	Process();

	EXPECT_EQ(uploadedUid, env->GetObjectID());
	EXPECT_EQ(m_scene.env_list.count(env->GetObjectID()), 1u);
}

TEST_F(SceneControllerTest, SceneObjectAdd_Camera_DoesNotEnqueueGpuUpload)
{
	bool uploaded = false;
	m_eventBus.subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested&) {
		uploaded = true;
	});

	auto camera = m_resources.Load<Camera>();
	m_eventBus.enqueue(SceneObjectAddRequested{camera->GetObjectID()});
	Process();

	EXPECT_FALSE(uploaded); // cameras own no GPU resources
	EXPECT_EQ(m_scene.cam_list.count(camera->GetObjectID()), 1u);
}

TEST_F(SceneControllerTest, LightShadowChanged_Enabled_EnqueuesGpuUpload)
{
	int uploadedUid = 0;
	m_eventBus.subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested& e) {
		uploadedUid = e.objectUid;
	});

	m_eventBus.enqueue(LightShadowChanged{m_light->GetObjectID(), true});
	Process();

	EXPECT_EQ(uploadedUid, m_light->GetObjectID());
}

TEST_F(SceneControllerTest, LightShadowChanged_Disabled_DoesNotEnqueueGpuUpload)
{
	bool uploaded = false;
	m_eventBus.subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested&) {
		uploaded = true;
	});

	m_eventBus.enqueue(LightShadowChanged{m_light->GetObjectID(), false});
	Process();

	EXPECT_FALSE(uploaded);
}

// --- Debug object properties (issue #22) -----------------------------------

/**
 * @brief SceneControllerTest plus one of each debug type registered.
 *
 * The three pools are unrelated types served by one event set, so every shared
 * knob is asserted on all three — that is the part a per-type handler would
 * have gotten wrong.
 */
class DebugPropertyTest : public SceneControllerTest
{
protected:
	void SetUp() override
	{
		SceneControllerTest::SetUp();

		m_dline = std::make_shared<DebugLine>();
		m_dpoints = std::make_shared<DebugPoints>();
		m_dmesh = std::make_shared<DebugMesh>();
		m_scene.UseDebugLine(m_dline);
		m_scene.UseDebugPoints(m_dpoints);
		m_scene.UseDebugMesh(m_dmesh);
	}

	std::shared_ptr<DebugLine> m_dline;
	std::shared_ptr<DebugPoints> m_dpoints;
	std::shared_ptr<DebugMesh> m_dmesh;
};

TEST_F(DebugPropertyTest, ColorChanged_AppliesToEveryDebugType)
{
	m_eventBus.enqueue(DebugColorChanged{m_dline->GetObjectID(), 1.0f, 0.0f, 0.0f, 1.0f});
	m_eventBus.enqueue(DebugColorChanged{m_dpoints->GetObjectID(), 0.0f, 1.0f, 0.0f, 0.5f});
	m_eventBus.enqueue(DebugColorChanged{m_dmesh->GetObjectID(), 0.0f, 0.0f, 1.0f, 0.25f});
	Process();

	EXPECT_EQ(m_dline->GetColor(), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
	EXPECT_EQ(m_dpoints->GetColor(), glm::vec4(0.0f, 1.0f, 0.0f, 0.5f));
	EXPECT_EQ(m_dmesh->GetColor(), glm::vec4(0.0f, 0.0f, 1.0f, 0.25f));
}
// PLACEHOLDER_DEBUG_TESTS

TEST_F(DebugPropertyTest, OpacityAndXRayChanged_AppliesToEveryDebugType)
{
	m_eventBus.enqueue(DebugOpacityChanged{m_dline->GetObjectID(), 0.25f});
	m_eventBus.enqueue(DebugOpacityChanged{m_dpoints->GetObjectID(), 0.5f});
	m_eventBus.enqueue(DebugOpacityChanged{m_dmesh->GetObjectID(), 0.75f});
	m_eventBus.enqueue(DebugXRayChanged{m_dline->GetObjectID(), true});
	m_eventBus.enqueue(DebugXRayChanged{m_dpoints->GetObjectID(), true});
	m_eventBus.enqueue(DebugXRayChanged{m_dmesh->GetObjectID(), true});
	Process();

	EXPECT_FLOAT_EQ(m_dline->GetOpacity(), 0.25f);
	EXPECT_FLOAT_EQ(m_dpoints->GetOpacity(), 0.5f);
	EXPECT_FLOAT_EQ(m_dmesh->GetOpacity(), 0.75f);
	EXPECT_TRUE(m_dline->GetXRay());
	EXPECT_TRUE(m_dpoints->GetXRay());
	EXPECT_TRUE(m_dmesh->GetXRay());
}

TEST_F(DebugPropertyTest, LineKnobsChanged_OnlyReachDebugLine)
{
	m_eventBus.enqueue(DebugLineWidthChanged{m_dline->GetObjectID(), 4.0f});
	m_eventBus.enqueue(DebugLineStippleChanged{m_dline->GetObjectID(), true});
	// Same events aimed at a DebugPoints must be ignored, not mis-applied.
	m_eventBus.enqueue(DebugLineWidthChanged{m_dpoints->GetObjectID(), 9.0f});
	Process();

	EXPECT_FLOAT_EQ(m_dline->GetWidth(), 4.0f);
	EXPECT_TRUE(m_dline->GetStipple());
}

TEST_F(DebugPropertyTest, PointKnobsChanged_OnlyReachDebugPoints)
{
	m_eventBus.enqueue(DebugPointTypeChanged{m_dpoints->GetObjectID(),
	                                         static_cast<int>(DebugPoints::PointType::CIR)});
	m_eventBus.enqueue(DebugPointScaleChanged{m_dpoints->GetObjectID(), 12.0f});
	m_eventBus.enqueue(DebugProjectionModeChanged{m_dpoints->GetObjectID(), 1});
	m_eventBus.enqueue(DebugPointScaleChanged{m_dline->GetObjectID(), 99.0f});
	Process();

	EXPECT_EQ(m_dpoints->GetPointType(), DebugPoints::PointType::CIR);
	EXPECT_FLOAT_EQ(m_dpoints->GetScale(), 12.0f);
	EXPECT_EQ(m_dpoints->GetProjectionMode(), 1);
}
// PLACEHOLDER_DEBUG_TESTS2

TEST_F(DebugPropertyTest, PositionsChanged_ReplacesTheWholeLineVertexList)
{
	m_dline->PushDebugLine(glm::vec3(0.0f), glm::vec3(1.0f));

	// Three triples: the absolute setter must shrink and grow alike.
	m_eventBus.enqueue(DebugPositionsChanged{m_dline->GetObjectID(),
	                                        { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f }});
	Process();

	ASSERT_EQ(m_dline->GetVertices().size(), 3u);
	EXPECT_EQ(m_dline->GetVertices()[0], glm::vec3(1.0f, 2.0f, 3.0f));
	EXPECT_EQ(m_dline->GetVertices()[2], glm::vec3(7.0f, 8.0f, 9.0f));
}

TEST_F(DebugPropertyTest, PositionsChanged_ReplacesTheWholePointList)
{
	m_eventBus.enqueue(DebugPositionsChanged{m_dpoints->GetObjectID(),
	                                        { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f }});
	Process();

	ASSERT_EQ(m_dpoints->GetPoints().size(), 2u);
	EXPECT_EQ(m_dpoints->GetPoints()[1], glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_F(DebugPropertyTest, PositionsChanged_DropsATrailingPartialTriple)
{
	// A flattened list is only ever built from whole positions, so a partial tail
	// is malformed input; it is dropped rather than padded with zeros.
	m_eventBus.enqueue(DebugPositionsChanged{m_dpoints->GetObjectID(),
	                                        { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f }});
	Process();

	ASSERT_EQ(m_dpoints->GetPoints().size(), 1u);
	EXPECT_EQ(m_dpoints->GetPoints()[0], glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_F(DebugPropertyTest, UnknownUid_IsIgnoredAndRecordsNothing)
{
	m_eventBus.enqueue(DebugColorChanged{999999, 1.0f, 0.0f, 0.0f, 1.0f});
	m_eventBus.enqueue(DebugPositionsChanged{999999, { 0.0f, 0.0f, 0.0f }});
	Process();

	EXPECT_FALSE(m_operations.CanUndo());
}
// PLACEHOLDER_DEBUG_TESTS3

TEST_F(DebugPropertyTest, ColorChanged_IsUndoableAndRedoable)
{
	const glm::vec4 before = m_dline->GetColor();

	m_eventBus.enqueue(DebugColorChanged{m_dline->GetObjectID(), 0.1f, 0.2f, 0.3f, 0.4f});
	Process();
	ASSERT_TRUE(m_operations.CanUndo());

	m_operations.Undo();
	EXPECT_EQ(m_dline->GetColor(), before);

	m_operations.Redo();
	EXPECT_EQ(m_dline->GetColor(), glm::vec4(0.1f, 0.2f, 0.3f, 0.4f));
}

TEST_F(DebugPropertyTest, PositionsChanged_IsUndoableAndRedoable)
{
	m_dpoints->PushDebugPoint(glm::vec3(5.0f, 5.0f, 5.0f));

	m_eventBus.enqueue(DebugPositionsChanged{m_dpoints->GetObjectID(),
	                                        { 1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f }});
	Process();
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
	m_eventBus.enqueue(DebugXRayChanged{m_dmesh->GetObjectID(), true});
	Process();
	ASSERT_TRUE(m_dmesh->GetXRay());

	m_operations.Undo();
	EXPECT_FALSE(m_dmesh->GetXRay());
}
