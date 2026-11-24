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

#include "command/Commands.hpp"
#include "core/ResourceManager.hpp"
#include "editor/Editor.hpp"
#include "editor/commands/EditorCommandAdapter.hpp"
#include "scene/Camera.hpp"
#include "scene/Light.hpp"
#include "scene/Mesh.hpp"
#include "scene/Scene.hpp"

#include <filesystem>
#include <fstream>
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

/**
 * @brief A missing file is reported as not found, not as an empty mesh.
 *
 * It also may not leave a MeshData behind: the pool is serialized by project
 * save, so a stale entry would make every later open of that project log a
 * load failure for a file that never existed.
 */
TEST_F(EditorCommandAdapterTest, ObjectLoadMissingFileIsNotFoundAndLeavesNoResource)
{
	const std::size_t poolBefore = m_editor->GetResourceManager().Size();
	const std::string missing =
		(std::filesystem::temp_directory_path() / "neurus_no_such_mesh.obj").string();

	const auto loaded = m_adapter->Execute({kObjectLoad, {{"path", missing}}});

	ASSERT_FALSE(loaded.ok);
	EXPECT_EQ(loaded.code, CommandErrorCode::IoError);
	EXPECT_NE(loaded.message.find("not found"), std::string::npos) << loaded.message;
	EXPECT_EQ(loaded.message.find("no geometry"), std::string::npos) << loaded.message;
	EXPECT_EQ(m_editor->GetResourceManager().Size(), poolBefore);
}

/**
 * @brief An existing OBJ with vertices but no faces fails as "no geometry" and
 *        must not stay in the pool either.
 */
TEST_F(EditorCommandAdapterTest, ObjectLoadEmptyObjLeavesNoResource)
{
	const std::filesystem::path path =
		std::filesystem::temp_directory_path() / "neurus_empty_mesh.obj";
	{
		std::ofstream out(path);
		ASSERT_TRUE(out.is_open());
		out << "o Empty\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"; // vertices, no faces
	}

	const std::size_t poolBefore = m_editor->GetResourceManager().Size();
	const auto loaded = m_adapter->Execute({kObjectLoad, {{"path", path.string()}}});
	std::error_code ignored;
	std::filesystem::remove(path, ignored);

	ASSERT_FALSE(loaded.ok);
	EXPECT_EQ(loaded.code, CommandErrorCode::IoError);
	EXPECT_NE(loaded.message.find("no geometry"), std::string::npos) << loaded.message;
	EXPECT_EQ(m_editor->GetResourceManager().Size(), poolBefore);
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
	const auto byOld = m_adapter->Execute({kObjectGet, {{"name", "Mesh"}}});
	EXPECT_FALSE(byOld.ok);
	EXPECT_EQ(byOld.code, CommandErrorCode::ObjectNotFound);

	const auto byNew = m_adapter->Execute({kObjectGet, {{"name", "Mesh A"}}});
	ASSERT_TRUE(byNew.ok) << byNew.message;
	EXPECT_EQ(byNew.details["object"]["name"], "Mesh A");
}

TEST_F(EditorCommandAdapterTest, MalformedObjectCommandsReportInvalidArgument)
{
	for (const CommandRequest& request : {
	         CommandRequest{kObjectSet, {{"name", "Mesh"}, {"fov", 50.0f}}},
	         CommandRequest{kObjectSet, {{"name", "Mesh"}}},
	         CommandRequest{kObjectGet, {}} })
	{
		SCOPED_TRACE(request.name + " " + request.args.dump());
		const auto result = m_adapter->Execute(request);
		EXPECT_FALSE(result.ok);
		EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument) << result.message;
	}
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

TEST_F(EditorCommandAdapterTest, MultiPropertySetPublishesOneUndoableChange)
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

	EXPECT_TRUE(m_editor->IsDirty());
	const auto read = m_adapter->Execute({kObjectGet, {{"name", "Mesh"}}});
	ASSERT_TRUE(read.ok) << read.message;
	EXPECT_EQ(result.details["object"]["position"], Json::array({4.0f, 5.0f, 6.0f}));
	EXPECT_EQ(read.details["object"]["position"], result.details["object"]["position"]);
	const auto history = m_adapter->Execute({kHistoryGet, {}});
	ASSERT_TRUE(history.ok) << history.message;
	EXPECT_TRUE(history.details["can_undo"]);
	EXPECT_EQ(history.details["undo"].back(), "Set Object");

	const auto undone = m_adapter->Execute({kHistoryUndo, {}});
	ASSERT_TRUE(undone.ok) << undone.message;
	EXPECT_TRUE(undone.details["can_redo"]);
	EXPECT_EQ(mesh->GetPosition(), glm::vec3(0.0f));
	EXPECT_EQ(mesh->GetRotation(), glm::vec3(0.0f));
	EXPECT_TRUE(mesh->is_viewport);
	EXPECT_TRUE(mesh->is_rendered);
	ASSERT_TRUE(m_adapter->Execute({kHistoryRedo, {}}).ok);
	EXPECT_EQ(mesh->GetPosition(), glm::vec3(4.0f, 5.0f, 6.0f));
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

TEST_F(EditorCommandAdapterTest, UnsupportedLightCreationCommandsIdentifyIssue60)
{
	const auto* light = FirstOfType(ObjectID::GOType::GO_LIGHT);
	ASSERT_NE(light, nullptr);
	for (const CommandRequest& request : {
	         CommandRequest{kObjectAdd, {{"type", "point-light"}}},
	         CommandRequest{kObjectDuplicate, {{"uid", light->GetObjectID()}}} })
	{
		SCOPED_TRACE(request.name);
		const auto result = m_adapter->Execute(request);
		EXPECT_FALSE(result.ok);
		EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
		EXPECT_NE(result.message.find("issue #60"), std::string::npos);
	}
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

TEST_F(EditorCommandAdapterTest, GraphStatusReportsBuiltInWithoutCreatingProjectData)
{
	const auto result = m_adapter->Execute({kRenderGraphStatus, {}});
	ASSERT_TRUE(result.ok) << result.message;
	const auto& graph = result.details.at("graph");
	EXPECT_EQ(graph.at("source"), "builtin");
	EXPECT_TRUE(graph.at("uid").is_null());
	EXPECT_EQ(graph.at("node_count"), 7);
	EXPECT_EQ(graph.at("edge_count"), 14);
	EXPECT_EQ(graph.at("outputs").at("color").at("node"), "fxaa");
	EXPECT_EQ(graph.at("outputs").at("object_id").at("port"), "gObjectID");
	EXPECT_TRUE(graph.at("diagnostics").empty());
	EXPECT_FALSE(m_editor->IsDirty());
}

TEST_F(EditorCommandAdapterTest, RenderCommandsReadDefaultsAndPublishEdits)
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

TEST_F(EditorCommandAdapterTest, HistoryUndoOnAnEmptyStackIsRejected)
{
	const auto result = m_adapter->Execute({kHistoryUndo, {}});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.code, CommandErrorCode::InvalidArgument);
}
