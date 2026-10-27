/**
 * @file test_scene_controller.cpp
 * @brief Scene document: selection and membership through SceneController.
 *
 * Object property writes are covered by test_object_controller.cpp - they
 * resolve through the resource pool, not the scene.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>

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
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "scene/ObjectID.h"
#include "core/RenderConfig.h"

using namespace neurus;

class SceneControllerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_controller.Init(m_ctx);

		// Pooled, like every object the app creates: membership events resolve the
		// UID through ctx.resources, so a non-pooled object would be invisible to
		// the add/delete flow.
		m_camera = m_resources.Load<Camera>();
		m_mesh   = std::make_shared<Mesh>();
		m_light  = std::make_shared<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
		m_scene.UseCamera(m_camera);
		m_scene.ActivateCamera(m_camera->GetObjectID());
		m_scene.UseMesh(m_mesh);
		m_scene.UseLight(m_light);
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
};

// --- Selection -------------------------------------------------------------

TEST_F(SceneControllerTest, ObjectSelected_SelectsObject)
{
	m_eventBus.Enqueue(ObjectSelected{m_mesh->GetObjectID(), 0});
	Process();
	EXPECT_TRUE(m_scene.selections.IsSelected(m_mesh.get()));
	EXPECT_EQ(m_scene.selections.GetActiveObject(), m_mesh.get());
}

TEST_F(SceneControllerTest, ObjectSelected_NullObject_ClearsSelection)
{
	m_scene.selections.Select(m_mesh.get(), false);
	m_eventBus.Enqueue(ObjectSelected{0, 0});
	Process();
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 0);
}

TEST_F(SceneControllerTest, ObjectSelected_Shift_AddsToSelection)
{
	m_eventBus.Enqueue(ObjectSelected{m_mesh->GetObjectID(), 0});
	m_eventBus.Enqueue(ObjectSelected{m_light->GetObjectID(),
	                                  static_cast<int>(Input::Mod_Shift)});
	Process();
	EXPECT_EQ(m_scene.selections.GetSelectionCount(), 2);
}

TEST_F(SceneControllerTest, ObjectDeselected_RemovesFromSelection)
{
	m_scene.selections.Select(m_mesh.get(), false);
	m_eventBus.Enqueue(ObjectDeselected{m_mesh->GetObjectID()});
	Process();
	EXPECT_FALSE(m_scene.selections.IsSelected(m_mesh.get()));
}

// --- Dirty / reset semantics ----------------------------------------------

TEST_F(SceneControllerTest, Selection_DoesNotEnqueueSceneModified)
{
	int modified = 0;
	m_eventBus.Subscribe<SceneModified>([&](const SceneModified&) { modified++; });
	m_eventBus.Enqueue(ObjectSelected{m_mesh->GetObjectID(), 0});
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

	m_eventBus.Enqueue(SceneObjectAddRequested{uid});
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

	// Redo: object re-registered (from the pool - no reload) + re-selected.
	m_operations.Redo();
	EXPECT_EQ(m_scene.mesh_list.count(uid), 1u);
	EXPECT_TRUE(m_scene.selections.IsSelected(m_scene.GetObjectID(uid)));
}

/**
 * @test Adding a pooled object registers its display name in the resource
 *       registry, so a runtime-created object is addressable by name without a
 *       save/load round-trip. Re-adding on undo/redo keeps the same name.
 */
TEST_F(SceneControllerTest, SceneObjectAdd_RegistersNameInResourceRegistry)
{
	auto mesh = m_resources.Load<Mesh>(m_resources.Load<MeshData>());
	m_eventBus.Enqueue(SceneObjectAddRequested{mesh->GetObjectID()});
	Process();

	ASSERT_FALSE(mesh->o_name.empty());
	EXPECT_EQ(m_resources.GetByName<Mesh>(mesh->o_name), mesh);

	// Undo removes the scene reference but not the pooled object; redo replays
	// the add, and the idempotent SetName must not suffix the owned name.
	const std::string name = mesh->o_name;
	m_operations.Undo();
	m_operations.Redo();
	EXPECT_EQ(mesh->o_name, name);
	EXPECT_EQ(m_resources.GetByName<Mesh>(name), mesh);
}

TEST_F(SceneControllerTest, SceneObjectAdd_AlreadyInScene_NoOp)
{
	const int uid = LoadPooledMesh(m_resources);
	m_eventBus.Enqueue(SceneObjectAddRequested{uid});
	Process();

	m_eventBus.Enqueue(SceneObjectAddRequested{uid});
	Process();
	EXPECT_EQ(m_scene.mesh_list.count(uid), 1u);
}

TEST_F(SceneControllerTest, SceneObjectAdd_StaleUid_NoOp)
{
	const int uid = LoadPooledMesh(m_resources);
	m_resources.Remove(uid); // resource no longer pooled

	m_eventBus.Enqueue(SceneObjectAddRequested{uid});
	Process();
	EXPECT_EQ(m_scene.mesh_list.count(uid), 0u);
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(SceneControllerTest, DeleteRequested_RemovesSelectionAndRecordsComposite)
{
	const int uid = LoadPooledMesh(m_resources);
	m_eventBus.Enqueue(SceneObjectAddRequested{uid});
	Process(); // mesh added + selected

	m_eventBus.Enqueue(ObjectDeleteRequested{});
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
	m_eventBus.Enqueue(SceneObjectAddRequested{uidA});
	m_eventBus.Enqueue(SceneObjectAddRequested{uidB});
	Process(); // both added; B selected last

	// Multi-select both (shift-add semantics).
	m_scene.selections.Select(m_scene.GetObjectID(uidA), true);

	m_eventBus.Enqueue(ObjectDeleteRequested{});
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

/**
 * @test Deleting the scene's only camera is allowed. There is no last-camera
 *       guard: a camera-less scene is a legal document, because the viewport
 *       looks through the viewport's own camera unless a scene camera is
 *       activated. The deletion is one undoable entry like any other, and the
 *       stale activation resolves to nullptr rather than to a dangling camera.
 */
TEST_F(SceneControllerTest, DeleteRequested_LastCamera_Allowed)
{
	m_scene.selections.Select(m_camera.get(), false);
	m_eventBus.Enqueue(ObjectDeleteRequested{});
	Process();

	EXPECT_EQ(m_scene.cam_list.count(m_camera->GetObjectID()), 0u);
	EXPECT_TRUE(m_scene.cam_list.empty());
	EXPECT_EQ(m_scene.GetActiveCamera(), nullptr);
	EXPECT_TRUE(m_operations.CanUndo());

	// Undo re-adds the camera, which re-validates the untouched activation uid.
	m_operations.Undo();
	Process();
	EXPECT_EQ(m_scene.cam_list.count(m_camera->GetObjectID()), 1u);
	EXPECT_EQ(m_scene.GetActiveCamera(), m_camera.get());
}

TEST_F(SceneControllerTest, DeleteRequested_EmptySelection_NoOp)
{
	m_eventBus.Enqueue(ObjectDeleteRequested{});
	Process();
	EXPECT_FALSE(m_operations.CanUndo());
}

TEST_F(SceneControllerTest, SceneObjectAdd_Light_EnqueuesLightingRebuild)
{
	bool rebuilt = false;
	m_eventBus.Subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });

	auto light = m_resources.Load<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
	m_eventBus.Enqueue(SceneObjectAddRequested{light->GetObjectID()});
	Process();
	EXPECT_TRUE(rebuilt);
	EXPECT_EQ(m_scene.light_list.count(light->GetObjectID()), 1u);
}

TEST_F(SceneControllerTest, DeleteRequested_Light_EnqueuesLightingRebuild)
{
	auto light = m_resources.Load<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
	m_eventBus.Enqueue(SceneObjectAddRequested{light->GetObjectID()});
	Process();

	bool rebuilt = false;
	m_eventBus.Subscribe<LightingRebuild>([&](const LightingRebuild&) { rebuilt = true; });
	m_eventBus.Enqueue(ObjectDeleteRequested{});
	Process();

	EXPECT_TRUE(rebuilt);
	EXPECT_EQ(m_scene.light_list.count(light->GetObjectID()), 0u);
}

// --- Scene-scoped GPU upload on demand (SceneObjectGpuUploadRequested) ------

TEST_F(SceneControllerTest, SceneObjectAdd_Mesh_EnqueuesGpuUpload)
{
	int uploadedUid = 0;
	m_eventBus.Subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested& e) {
		uploadedUid = e.objectUid;
	});

	auto mesh = m_resources.Load<Mesh>(m_resources.Load<MeshData>());
	m_eventBus.Enqueue(SceneObjectAddRequested{mesh->GetObjectID()});
	Process();

	EXPECT_EQ(uploadedUid, mesh->GetObjectID()); // the re-added mesh
	EXPECT_EQ(m_scene.mesh_list.count(mesh->GetObjectID()), 1u);
}

TEST_F(SceneControllerTest, SceneObjectAdd_Light_EnqueuesGpuUpload)
{
	int uploadedUid = 0;
	m_eventBus.Subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested& e) {
		uploadedUid = e.objectUid;
	});

	auto light = m_resources.Load<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
	m_eventBus.Enqueue(SceneObjectAddRequested{light->GetObjectID()});
	Process();

	EXPECT_EQ(uploadedUid, light->GetObjectID());
	EXPECT_EQ(m_scene.light_list.count(light->GetObjectID()), 1u);
}

TEST_F(SceneControllerTest, SceneObjectAdd_Environment_EnqueuesGpuUpload)
{
	int uploadedUid = 0;
	m_eventBus.Subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested& e) {
		uploadedUid = e.objectUid;
	});

	auto env = m_resources.Load<Environment>(m_resources.Load<ImageData>(""));
	m_eventBus.Enqueue(SceneObjectAddRequested{env->GetObjectID()});
	Process();

	EXPECT_EQ(uploadedUid, env->GetObjectID());
	EXPECT_EQ(m_scene.env_list.count(env->GetObjectID()), 1u);
}

TEST_F(SceneControllerTest, SceneObjectAdd_Camera_DoesNotEnqueueGpuUpload)
{
	bool uploaded = false;
	m_eventBus.Subscribe<SceneObjectGpuUploadRequested>([&](const SceneObjectGpuUploadRequested&) {
		uploaded = true;
	});

	auto camera = m_resources.Load<Camera>();
	m_eventBus.Enqueue(SceneObjectAddRequested{camera->GetObjectID()});
	Process();

	EXPECT_FALSE(uploaded); // cameras own no GPU resources
	EXPECT_EQ(m_scene.cam_list.count(camera->GetObjectID()), 1u);
}
