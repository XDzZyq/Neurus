/**
 * @file test_scene_camera_activation.cpp
 * @brief Which scene camera the viewport looks through is explicit state the
 *        Scene owns — not whichever camera the hash order happens to yield.
 *
 * GetActiveCamera() used to return `cam_list.begin()->second`, so "the active
 * camera" was an artifact of unordered_map bucketing: registering a second
 * camera could silently steal the view, and a scene had to contain a camera or
 * nothing rendered at all. It now resolves ActiveCameraID() against cam_list,
 * and **no camera is activated by default** — the Editor owns the camera the
 * viewport uses, so "none" is the normal state rather than a failure.
 *
 * Two contracts here are subtle enough to be worth pinning:
 *
 * - **The UID is deliberately left stale when a camera is removed.** Deleting
 *   the activated camera makes GetActiveCamera() return nullptr while
 *   ActiveCameraID() still names it, so undoing the delete re-validates the
 *   activation for free and the whole gesture stays one undo entry. Only
 *   GetActiveCamera() validates; ActiveCameraID() is for UID comparison alone.
 *
 * - **Absent and none must stay distinguishable on load.** A file written
 *   before the field existed migrates once (activating the camera the old
 *   positional lookup would have picked, so an upgraded project opens through
 *   the camera it was saved with); a file that explicitly says "none" must load
 *   as none and never migrate.
 *
 * Pure CPU — no GPU, no Vulkan.
 */

#include <gtest/gtest.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "asset/Project.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/components/SceneComponent.hpp"
#include "asset/data/MeshData.hpp"
#include "asset/ResourceManager.hpp"
#include "scene/Camera.hpp"
#include "scene/Mesh.hpp"
#include "scene/Scene.hpp"

// Force-link the polymorphic registration TUs (static libs).
#include "asset/registrations/DataRegistration.hpp"
#include "scene/registrations/TypeRegistration.hpp"

using namespace neurus;

namespace
{

struct TempFile
{
	std::string path;
	explicit TempFile(std::string p) : path(std::move(p)) {}
	~TempFile() { std::remove(path.c_str()); }
};

/// @brief Reads a whole file, for the absent-selection test.
std::string ReadAll(const std::string& path)
{
	std::ifstream in(path);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

/**
 * @brief Deletes the `"activeCamUid": N` entry from a saved project, producing
 *        a document without a saved activation.
 *
 * Done by editing a real save rather than hand-authoring JSON: the field is the
 * last one in the m_scene node and its position in the archive is frozen
 * forever, so a hand-written stand-in would drift from the real format the
 * moment anything else is appended. Walks back to the separating comma and
 * forward past the number, so pretty-printing and spacing do not matter.
 */
bool StripActiveCamUid(const std::string& path)
{
	std::string json = ReadAll(path);
	const size_t key = json.find("\"activeCamUid\"");
	if (key == std::string::npos)
		return false;

	const size_t comma = json.rfind(',', key);
	if (comma == std::string::npos)
		return false;

	size_t end = json.find(':', key);
	if (end == std::string::npos)
		return false;
	++end;
	while (end < json.size() && (std::isspace(static_cast<unsigned char>(json[end]))
	                             || json[end] == '-' || std::isdigit(static_cast<unsigned char>(json[end]))))
		++end;

	json.erase(comma, end - comma);
	std::ofstream out(path);
	out << json;
	return true;
}

} // namespace

TEST(SceneCameraActivation, ActivateCamera_PutsThatExactCameraInUse)
{
	Scene scene;
	ResourceManager resources;

	auto first = resources.Load<Camera>();
	auto second = resources.Load<Camera>();
	scene.UseCamera(first);
	scene.UseCamera(second);
	EXPECT_EQ(scene.ActiveCameraID(), 0);
	const Scene& view = scene;
	EXPECT_EQ(view.GetActiveCamera(), nullptr);
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);

	ASSERT_TRUE(scene.ActivateCamera(second->GetObjectID()));
	EXPECT_EQ(scene.ActiveCameraID(), second->GetObjectID());
	EXPECT_EQ(scene.GetActiveCamera(), second.get());
	EXPECT_EQ(view.GetActiveCamera(), second.get());

	// Replacing, not accumulating: the previous activation is simply dropped.
	ASSERT_TRUE(scene.ActivateCamera(first->GetObjectID()));
	EXPECT_EQ(scene.GetActiveCamera(), first.get());
	ASSERT_TRUE(scene.RemoveCamera(second->GetObjectID()));
	EXPECT_EQ(scene.GetActiveCamera(), first.get());
	scene.DeactivateCamera();
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);
	EXPECT_EQ(scene.ActiveCameraID(), 0);
	EXPECT_EQ(scene.cam_list.size(), 1u);
}

TEST(SceneCameraActivation, ActivateCamera_RejectsAPooledNonSceneCamera)
{
	Scene scene;
	ResourceManager resources;

	auto sceneCam = resources.Load<Camera>();
	scene.UseCamera(sceneCam);
	ASSERT_TRUE(scene.ActivateCamera(sceneCam->GetObjectID()));

	auto editorCam = resources.Load<Camera>(); // pooled, never registered
	EXPECT_FALSE(scene.ActivateCamera(editorCam->GetObjectID()));
	EXPECT_FALSE(scene.ActivateCamera(0));
	EXPECT_FALSE(scene.ActivateCamera(999999));

	// A refused activation changes nothing.
	EXPECT_EQ(scene.ActiveCameraID(), sceneCam->GetObjectID());
	EXPECT_EQ(scene.GetActiveCamera(), sceneCam.get());
}

TEST(SceneCameraActivation, ReAddingTheRemovedCamera_RestoresTheActivation)
{
	Scene scene;
	ResourceManager resources;

	auto cam = resources.Load<Camera>();
	const int uid = cam->GetObjectID();
	scene.UseCamera(cam);
	ASSERT_TRUE(scene.ActivateCamera(uid));
	ASSERT_TRUE(scene.RemoveCamera(uid));
	EXPECT_TRUE(scene.cam_list.empty());
	EXPECT_EQ(scene.ActiveCameraID(), uid);
	ASSERT_EQ(scene.GetActiveCamera(), nullptr);

	scene.UseCamera(cam); // the undo of the delete
	EXPECT_EQ(scene.GetActiveCamera(), cam.get())
		<< "the surviving UID must re-resolve without a second activation";
}

// ---------------------------------------------------------------------------
// Activation and selection are independent axes
// ---------------------------------------------------------------------------

/**
 * @test Selecting a camera does not put it in use, and activating one does not
 *       select it. They are deliberately separate: the PropertyPanel inspects
 *       whatever is *selected* while the viewport looks through whatever is
 *       *activated*, so a user can adjust one camera while looking through
 *       another.
 */
TEST(SceneCameraActivation, SelectionAndActivationDoNotFollowEachOther)
{
	Scene scene;
	ResourceManager resources;

	auto viewed = resources.Load<Camera>();
	auto inspected = resources.Load<Camera>();
	scene.UseCamera(viewed);
	scene.UseCamera(inspected);
	ASSERT_TRUE(scene.ActivateCamera(viewed->GetObjectID()));

	scene.selections.Select(inspected.get(), false);

	EXPECT_EQ(scene.GetActiveCamera(), viewed.get()) << "selection must not steal the view";
	const ObjectID* active = scene.selections.GetActiveObject();
	ASSERT_NE(active, nullptr);
	EXPECT_EQ(active->GetObjectID(), inspected->GetObjectID())
		<< "activation must not move the selection";
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

namespace
{

/// @brief Saves a two-camera scene, activating @p activateSecond ? the second
///        camera : nothing. Returns the two UIDs in registration order.
void SaveTwoCameraScene(const std::string& path, bool activateSecond,
                        int& outFirstUid, int& outSecondUid)
{

	Scene scene;
	ResourceManager resources;

	auto first = resources.Load<Camera>();
	auto second = resources.Load<Camera>();
	scene.UseCamera(first);
	scene.UseCamera(second);
	outFirstUid = first->GetObjectID();
	outSecondUid = second->GetObjectID();

	if (activateSecond)
		ASSERT_TRUE(scene.ActivateCamera(outSecondUid));

	project::Project p;
	p.Register<project::ResourceComponent>(resources);
	p.Register<project::SceneComponent>(scene, resources);
	p.Save(path);
}

/// @brief Loads a project into @p scene, which must outlive @p resources' users.
void LoadProject(const std::string& path, Scene& scene, ResourceManager& resources)
{

	project::Project p;
	p.Register<project::ResourceComponent>(resources);
	p.Register<project::SceneComponent>(scene, resources);
	p.Load(path);
}

} // namespace

/**
 * @test The activated camera survives a save/load: it is the *second* registered
 *       camera, so a load that fell back to positional order would have a 50%
 *       chance of looking right. Asserting the exact UID is what makes this test
 *       independent of unordered_map bucketing.
 */
TEST(SceneCameraActivation, ActivationRoundTripsThroughAProjectFile)
{
	TempFile tmp("test_scene_camera_activation_rt.neurus.json");

	int firstUid = 0;
	int secondUid = 0;
	SaveTwoCameraScene(tmp.path, /*activateSecond=*/true, firstUid, secondUid);
	ASSERT_NE(firstUid, secondUid);

	Scene loaded;
	ResourceManager loadedResources;
	LoadProject(tmp.path, loaded, loadedResources);

	ASSERT_EQ(loaded.cam_list.size(), 2u);
	EXPECT_EQ(loaded.ActiveCameraID(), secondUid);
	Camera* cam = loaded.GetActiveCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->GetObjectID(), secondUid);
}

/// An explicitly unselected scene camera keeps the viewport active.
TEST(SceneCameraActivation, ExplicitNoneRoundTripsAsNone)
{
	TempFile tmp("test_scene_camera_activation_none.neurus.json");

	int firstUid = 0;
	int secondUid = 0;
	SaveTwoCameraScene(tmp.path, /*activateSecond=*/false, firstUid, secondUid);

	Scene loaded;
	ResourceManager loadedResources;
	LoadProject(tmp.path, loaded, loadedResources);

	ASSERT_EQ(loaded.cam_list.size(), 2u);
	EXPECT_EQ(loaded.ActiveCameraID(), 0);
	EXPECT_EQ(loaded.GetActiveCamera(), nullptr);
}

TEST(SceneCameraActivation, AbsentSelectionLeavesViewportActive)
{
	TempFile tmp("test_scene_camera_activation_legacy.neurus.json");

	int firstUid = 0;
	int secondUid = 0;
	SaveTwoCameraScene(tmp.path, /*activateSecond=*/true, firstUid, secondUid);
	ASSERT_TRUE(StripActiveCamUid(tmp.path)) << "the save must contain the field to strip";

	Scene loaded;
	ResourceManager loadedResources;
	LoadProject(tmp.path, loaded, loadedResources);

	ASSERT_EQ(loaded.cam_list.size(), 2u);
	EXPECT_EQ(loaded.ActiveCameraID(), 0);
	EXPECT_EQ(loaded.GetActiveCamera(), nullptr);
}

TEST(SceneCameraActivation, AbsentSelectionWithNoCamerasLeavesViewportActive)
{

	TempFile tmp("test_scene_camera_activation_legacy_empty.neurus.json");

	{
		Scene scene;
		ResourceManager resources;
		scene.UseMesh(resources.Load<Mesh>(resources.Load<MeshData>("res/obj/sphere.obj")));

		project::Project p;
		p.Register<project::ResourceComponent>(resources);
		p.Register<project::SceneComponent>(scene, resources);
		p.Save(tmp.path);
	}
	ASSERT_TRUE(StripActiveCamUid(tmp.path));

	Scene loaded;
	ResourceManager loadedResources;
	LoadProject(tmp.path, loaded, loadedResources);

	EXPECT_TRUE(loaded.cam_list.empty());
	EXPECT_EQ(loaded.ActiveCameraID(), 0);
	EXPECT_EQ(loaded.GetActiveCamera(), nullptr);
	EXPECT_EQ(loaded.mesh_list.size(), 1u) << "the rest of the document must still load";
}
