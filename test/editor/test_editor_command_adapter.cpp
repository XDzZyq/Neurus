/**
 * @file test_editor_command_adapter.cpp
 * @brief Scene/object command reads and writes against a headless editor.
 *
 * No GPU: Editor(nullptr) skips uploads, while Initialize() still wires the
 * controllers that apply the typed events the adapter emits. This is the same
 * event path a UI edit takes, so the test also pins the command layer to the
 * production mutation and undo behavior.
 */

#include <gtest/gtest.h>

#include "command/Commands.h"
#include "editor/Editor.h"
#include "editor/commands/EditorCommandAdapter.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include <memory>
#include <string>

using namespace neurus;
using namespace neurus::command;

namespace
{

class EditorCommandAdapterTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_editor = std::make_unique<Editor>(nullptr);
		m_editor->Initialize();
		m_editor->NewScene();
		m_editor->CreateDefaultScene("res/obj/sphere.obj");
		m_editor->ClearDirty();
		m_adapter = std::make_unique<EditorCommandAdapter>(*m_editor);
	}

	ObjectID* FirstOfType(ObjectID::GOType type)
	{
		for (auto& [uid, object] : m_editor->GetScene().obj_list)
		{
			(void)uid;
			if (object->o_type == type)
				return object.get();
		}
		return nullptr;
	}

	std::unique_ptr<Editor> m_editor;
	std::unique_ptr<EditorCommandAdapter> m_adapter;
};

} // namespace

TEST_F(EditorCommandAdapterTest, SceneListReportsDefaultContent)
{
	const auto all = m_adapter->Execute({kSceneList, {}});
	ASSERT_TRUE(all.ok) << all.message;
	EXPECT_GT(all.details.value("count", 0), 0);
	for (const auto& object : all.details["objects"])
	{
		EXPECT_TRUE(object.contains("uid"));
		EXPECT_TRUE(object.contains("name"));
		EXPECT_TRUE(object.contains("type"));
	}

	const auto meshes = m_adapter->Execute({kSceneList, {{"type", "mesh"}}});
	ASSERT_TRUE(meshes.ok) << meshes.message;
	ASSERT_EQ(meshes.details["count"], 1);
	EXPECT_EQ(meshes.details["objects"][0]["type"], "mesh");
	EXPECT_TRUE(meshes.details["objects"][0].contains("mesh_path"));

	const auto lights = m_adapter->Execute({kSceneList, {{"type", "light"}}});
	ASSERT_TRUE(lights.ok) << lights.message;
	ASSERT_EQ(lights.details["count"], 1);
	EXPECT_EQ(lights.details["objects"][0]["light_type"], "point");
}

TEST_F(EditorCommandAdapterTest, UnknownTypeFilterIsRejected)
{
	const auto result = m_adapter->Execute({kSceneList, {{"type", "bogus"}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}

TEST_F(EditorCommandAdapterTest, ObjectLoadAddsANamedMesh)
{
	const int before = static_cast<int>(m_editor->GetScene().mesh_list.size());
	const auto loaded = m_adapter->Execute(
		{kObjectLoad, {{"path", "res/obj/plane.obj"}, {"name", "Plane A"}}});
	ASSERT_TRUE(loaded.ok) << loaded.message;
	EXPECT_EQ(loaded.details["object"]["name"], "Plane A");
	EXPECT_EQ(loaded.details["object"]["type"], "mesh");
	EXPECT_EQ(m_editor->GetScene().mesh_list.size(), static_cast<std::size_t>(before + 1));
	EXPECT_NE(m_editor->GetScene().GetObjectID(loaded.details["object"]["uid"].get<int>()),
	          nullptr);
}

TEST_F(EditorCommandAdapterTest, ObjectSetUpdatesPositionAndReadBackAgrees)
{
	const auto set = m_adapter->Execute(
		{kObjectSet, {{"name", "Mesh"}, {"position", {1.0f, 2.0f, 3.0f}}}});
	ASSERT_TRUE(set.ok) << set.message;
	EXPECT_EQ(set.details["object"]["position"][0], 1.0f);
	EXPECT_EQ(set.details["object"]["position"][1], 2.0f);
	EXPECT_EQ(set.details["object"]["position"][2], 3.0f);

	Mesh* mesh = dynamic_cast<Mesh*>(FirstOfType(ObjectID::GOType::GO_MESH));
	ASSERT_NE(mesh, nullptr);
	EXPECT_EQ(mesh->GetPosition(), glm::vec3(1.0f, 2.0f, 3.0f));
	EXPECT_TRUE(m_editor->IsDirty());

	const auto get = m_adapter->Execute({kObjectGet, {{"name", "Mesh"}}});
	ASSERT_TRUE(get.ok) << get.message;
	EXPECT_EQ(get.details["object"]["position"][2], 3.0f);
}

TEST_F(EditorCommandAdapterTest, ObjectSetRenamesAndResolvesByTheNewName)
{
	const auto rename = m_adapter->Execute({kObjectSet, {{"name", "Mesh"}, {"rename", "Mesh A"}}});
	ASSERT_TRUE(rename.ok) << rename.message;
	EXPECT_EQ(rename.details["object"]["name"], "Mesh A");

	const auto byOld = m_adapter->Execute({kObjectGet, {{"name", "Mesh"}}});
	EXPECT_FALSE(byOld.ok);
	EXPECT_EQ(byOld.code, CommandErrorCode::ObjectNotFound);

	const auto byNew = m_adapter->Execute({kObjectGet, {{"name", "Mesh A"}}});
	ASSERT_TRUE(byNew.ok) << byNew.message;
	EXPECT_EQ(byNew.details["object"]["name"], "Mesh A");
}

TEST_F(EditorCommandAdapterTest, ObjectSetRenameIsUndoable)
{
	Mesh* mesh = dynamic_cast<Mesh*>(FirstOfType(ObjectID::GOType::GO_MESH));
	ASSERT_NE(mesh, nullptr);
	const std::size_t before = m_editor->GetOperations().GetUndoStack().size();

	ASSERT_TRUE(m_adapter->Execute(
		{kObjectSet, {{"name", "Mesh"}, {"rename", "Mesh A"}}}).ok);
	EXPECT_EQ(mesh->o_name, "Mesh A");
	EXPECT_EQ(m_editor->GetOperations().GetUndoStack().size(), before + 1);

	ASSERT_TRUE(m_adapter->Execute({kHistoryUndo, {}}).ok);
	EXPECT_EQ(mesh->o_name, "Mesh");
	ASSERT_TRUE(m_adapter->Execute({kHistoryRedo, {}}).ok);
	EXPECT_EQ(mesh->o_name, "Mesh A");
}

TEST_F(EditorCommandAdapterTest, TypeSpecificWriteOnTheWrongObjectIsRejected)
{
	const auto result = m_adapter->Execute({kObjectSet, {{"name", "Mesh"}, {"fov", 50.0f}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}

TEST_F(EditorCommandAdapterTest, ObjectDeleteRemovesSceneMembershipButKeepsThePool)
{
	const int uid = FirstOfType(ObjectID::GOType::GO_MESH)->GetObjectID();
	const auto deleted = m_adapter->Execute({kObjectDelete, {{"uid", uid}}});
	ASSERT_TRUE(deleted.ok) << deleted.message;
	EXPECT_EQ(deleted.details["count"], 1);
	EXPECT_EQ(m_editor->GetScene().GetObjectID(uid), nullptr);
	EXPECT_NE(m_editor->GetResourceManager().Get<Mesh>(uid), nullptr);
}

TEST_F(EditorCommandAdapterTest, MultiPropertySetIsOneUndoEntryAndUndoRevertsAll)
{
	Mesh* mesh = dynamic_cast<Mesh*>(FirstOfType(ObjectID::GOType::GO_MESH));
	ASSERT_NE(mesh, nullptr);
	const std::size_t before = m_editor->GetOperations().GetUndoStack().size();

	const auto result = m_adapter->Execute(
		{kObjectSet,
		 {{"name", "Mesh"},
		  {"position", {4.0f, 5.0f, 6.0f}},
		  {"rotation", {10.0f, 20.0f, 30.0f}},
		  {"visible", false}}});
	ASSERT_TRUE(result.ok) << result.message;
	EXPECT_EQ(m_editor->GetOperations().GetUndoStack().size(), before + 1);
	EXPECT_EQ(mesh->GetPosition(), glm::vec3(4.0f, 5.0f, 6.0f));
	EXPECT_EQ(mesh->GetRotation(), glm::vec3(10.0f, 20.0f, 30.0f));
	EXPECT_FALSE(mesh->is_viewport);
	EXPECT_FALSE(mesh->is_rendered);

	m_editor->GetOperations().Undo();
	EXPECT_EQ(mesh->GetPosition(), glm::vec3(0.0f));
	EXPECT_EQ(mesh->GetRotation(), glm::vec3(0.0f));
	EXPECT_TRUE(mesh->is_viewport);
	EXPECT_TRUE(mesh->is_rendered);
}

TEST_F(EditorCommandAdapterTest, SceneInfoReportsCountsAndSelection)
{
	const auto result = m_adapter->Execute({kSceneInfo, {}});
	ASSERT_TRUE(result.ok) << result.message;
	EXPECT_EQ(result.details["meshes"], 1);
	EXPECT_EQ(result.details["lights"], 1);
	EXPECT_EQ(result.details["cameras"], 1);
	EXPECT_EQ(result.details["environments"], 1);
	EXPECT_GT(result.details["debug"].get<int>(), 0);
	EXPECT_FALSE(result.details["dirty"].get<bool>());
}

TEST_F(EditorCommandAdapterTest, ObjectAddCreatesNamedCameraAsOneUndoEntry)
{
	const std::size_t before = m_editor->GetOperations().GetUndoStack().size();
	const auto result = m_adapter->Execute(
		{kObjectAdd,
		 {{"type", "camera"},
		  {"name", "Camera A"},
		  {"position", {1.0f, 2.0f, 3.0f}},
		  {"fov", 55.0f}}});
	ASSERT_TRUE(result.ok) << result.message;
	EXPECT_EQ(result.details["object"]["name"], "Camera A");
	EXPECT_EQ(result.details["object"]["type"], "camera");
	EXPECT_EQ(result.details["object"]["fov"], 55.0f);
	EXPECT_EQ(result.details["object"]["position"][0], 1.0f);
	EXPECT_EQ(m_editor->GetScene().cam_list.size(), 2u);
	EXPECT_EQ(m_editor->GetOperations().GetUndoStack().size(), before + 1);
}

TEST_F(EditorCommandAdapterTest, ObjectAddRejectsLightsWhileIssue60IsOpen)
{
	const auto result = m_adapter->Execute({kObjectAdd, {{"type", "point-light"}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
	EXPECT_NE(result.message.find("issue #60"), std::string::npos);
}

TEST_F(EditorCommandAdapterTest, ObjectDuplicateCopiesMeshAndCamera)
{
	Mesh* mesh = dynamic_cast<Mesh*>(FirstOfType(ObjectID::GOType::GO_MESH));
	Camera* camera = dynamic_cast<Camera*>(FirstOfType(ObjectID::GOType::GO_CAM));
	ASSERT_NE(mesh, nullptr);
	ASSERT_NE(camera, nullptr);

	mesh->SetPosition(glm::vec3(4.0f, 5.0f, 6.0f));
	const std::size_t meshCount = m_editor->GetScene().mesh_list.size();
	const auto meshCopy = m_adapter->Execute(
		{kObjectDuplicate, {{"uid", mesh->GetObjectID()}, {"rename", "Mesh Copy"}}});
	ASSERT_TRUE(meshCopy.ok) << meshCopy.message;
	EXPECT_EQ(meshCopy.details["object"]["name"], "Mesh Copy");
	EXPECT_EQ(meshCopy.details["object"]["position"][0], 4.0f);
	EXPECT_EQ(m_editor->GetScene().mesh_list.size(), meshCount + 1);

	const std::size_t cameraCount = m_editor->GetScene().cam_list.size();
	const auto cameraCopy = m_adapter->Execute(
		{kObjectDuplicate, {{"uid", camera->GetObjectID()}}});
	ASSERT_TRUE(cameraCopy.ok) << cameraCopy.message;
	EXPECT_EQ(cameraCopy.details["object"]["type"], "camera");
	EXPECT_EQ(cameraCopy.details["object"]["name"], std::string("Camera Copy"));
	EXPECT_EQ(m_editor->GetScene().cam_list.size(), cameraCount + 1);
}

TEST_F(EditorCommandAdapterTest, ObjectDuplicateRejectsLights)
{
	ObjectID* light = FirstOfType(ObjectID::GOType::GO_LIGHT);
	ASSERT_NE(light, nullptr);
	const auto result = m_adapter->Execute(
		{kObjectDuplicate, {{"uid", light->GetObjectID()}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
	EXPECT_NE(result.message.find("issue #60"), std::string::npos);
}

TEST_F(EditorCommandAdapterTest, ObjectDeleteAcceptsAUidList)
{	const int mesh = FirstOfType(ObjectID::GOType::GO_MESH)->GetObjectID();
	const int light = FirstOfType(ObjectID::GOType::GO_LIGHT)->GetObjectID();
	const auto deleted = m_adapter->Execute(
		{kObjectDelete, {{"uids", {mesh, light}}}});
	ASSERT_TRUE(deleted.ok) << deleted.message;
	EXPECT_EQ(deleted.details["count"], 2);
	EXPECT_EQ(m_editor->GetScene().GetObjectID(mesh), nullptr);
	EXPECT_EQ(m_editor->GetScene().GetObjectID(light), nullptr);
}

TEST_F(EditorCommandAdapterTest, RenderGetReportsTheDefaults)
{
	const auto result = m_adapter->Execute({kRenderGet, {}});
	ASSERT_TRUE(result.ok) << result.message;
	const auto& config = result.details["config"];
	EXPECT_EQ(config["pipeline"], "deferred");
	EXPECT_EQ(config["aa"], "none");
	EXPECT_EQ(config["ao"], "ssao");
	EXPECT_EQ(config["shadow"], "shadow_mapping");
	EXPECT_EQ(config["transparent"], false);
	EXPECT_EQ(config["debug_draw"], true);
}

TEST_F(EditorCommandAdapterTest, RenderSetChangesAndReadsBack)
{
	const auto set = m_adapter->Execute(
		{kRenderSet,
		 {{"pipeline", "deferred"},
		  {"aa", "fxaa"},
		  {"gamma", 1.5f},
		  {"transparent", true},
		  {"ao_ksize", 8}}});
	ASSERT_TRUE(set.ok) << set.message;
	EXPECT_EQ(set.details["config"]["aa"], "fxaa");
	EXPECT_FLOAT_EQ(set.details["config"]["gamma"].get<float>(), 1.5f);
	EXPECT_EQ(set.details["config"]["transparent"], true);
	EXPECT_EQ(set.details["config"]["ao_ksize"], 8);

	const auto get = m_adapter->Execute({kRenderGet, {}});
	ASSERT_TRUE(get.ok) << get.message;
	EXPECT_EQ(get.details["config"]["aa"], "fxaa");
	EXPECT_EQ(get.details["config"]["transparent"], true);
	EXPECT_TRUE(m_editor->IsDirty() == false)
		<< "RenderConfig edits should not mark the scene document dirty.";
}

TEST_F(EditorCommandAdapterTest, RenderSetRejectsUnknownEnumAndBadRange)
{
	const auto unknown = m_adapter->Execute({kRenderSet, {{"aa", "bogus"}}});
	EXPECT_FALSE(unknown.ok);
	EXPECT_EQ(unknown.code, CommandErrorCode::InvalidArgument);

	const auto badRange = m_adapter->Execute({kRenderSet, {{"gamma", 0.0f}}});
	EXPECT_FALSE(badRange.ok);
	EXPECT_EQ(badRange.code, CommandErrorCode::InvalidArgument);
}

TEST_F(EditorCommandAdapterTest, HistoryUndoRestoresThePreviousTransform)
{
	ASSERT_TRUE(m_adapter->Execute(
		{kObjectSet, {{"name", "Mesh"}, {"position", {7.0f, 8.0f, 9.0f}}}}).ok);

	const auto before = m_adapter->Execute({kHistoryGet, {}});
	ASSERT_TRUE(before.ok) << before.message;
	EXPECT_TRUE(before.details["can_undo"]);
	EXPECT_EQ(before.details["undo"].back(), "Set Object");

	const auto undone = m_adapter->Execute({kHistoryUndo, {}});
	ASSERT_TRUE(undone.ok) << undone.message;
	EXPECT_TRUE(undone.details["can_redo"]);

	Mesh* mesh = dynamic_cast<Mesh*>(FirstOfType(ObjectID::GOType::GO_MESH));
	ASSERT_NE(mesh, nullptr);
	EXPECT_EQ(mesh->GetPosition(), glm::vec3(0.0f));

	const auto redone = m_adapter->Execute({kHistoryRedo, {}});
	ASSERT_TRUE(redone.ok) << redone.message;
	EXPECT_EQ(mesh->GetPosition(), glm::vec3(7.0f, 8.0f, 9.0f));
}

TEST_F(EditorCommandAdapterTest, HistoryUndoOnAnEmptyStackIsRejected)
{
	const auto result = m_adapter->Execute({kHistoryUndo, {}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}

TEST_F(EditorCommandAdapterTest, MissingObjectIsReported)
{
	const auto result = m_adapter->Execute({kObjectGet, {{"name", "Does Not Exist"}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::ObjectNotFound);
}

TEST_F(EditorCommandAdapterTest, NoTargetIsRejected)
{
	const auto result = m_adapter->Execute({kObjectGet, {}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}

TEST_F(EditorCommandAdapterTest, ObjectSetWithoutAPropertyIsRejected)
{
	const auto result = m_adapter->Execute({kObjectSet, {{"name", "Mesh"}}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}
