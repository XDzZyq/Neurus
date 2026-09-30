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

#include "asset/Project.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/SceneComponent.h"
#include "asset/data/MeshData.h"
#include "core/ResourceManager.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

// Force-link the polymorphic registration TUs (static libs).
#include "asset/registrations/DataRegistration.h"
#include "scene/registrations/TypeRegistration.h"

using namespace neurus;

namespace
{

struct TempFile
{
	std::string path;
	explicit TempFile(std::string p) : path(std::move(p)) {}
	~TempFile() { std::remove(path.c_str()); }
};

/// @brief Reads a whole file, for the legacy-migration test's JSON surgery.
std::string ReadAll(const std::string& path)
{
	std::ifstream in(path);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

/**
 * @brief Deletes the `"activeCamUid": N` entry from a saved project, producing
 *        the exact file an older build would have written.
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

// ---------------------------------------------------------------------------
// Default state: registering cameras activates nothing
// ---------------------------------------------------------------------------

/**
 * @test Two registered cameras leave the scene with none activated. This is the
 *       whole point of the feature: under the old positional lookup the first
 *       bucket won, so a scene could never report "no camera in use".
 */
TEST(SceneCameraActivation, RegisteringCamerasActivatesNothing)
{
	Scene scene;
	ResourceManager resources;

	scene.UseCamera(resources.Load<Camera>());
	scene.UseCamera(resources.Load<Camera>());

	ASSERT_EQ(scene.cam_list.size(), 2u);
	EXPECT_EQ(scene.ActiveCameraID(), 0);
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);
	EXPECT_EQ(static_cast<const Scene&>(scene).GetActiveCamera(), nullptr);
}

// ---------------------------------------------------------------------------
// Activate / replace / deactivate
// ---------------------------------------------------------------------------

TEST(SceneCameraActivation, ActivateCamera_PutsThatExactCameraInUse)
{
	Scene scene;
	ResourceManager resources;

	auto first = resources.Load<Camera>();
	auto second = resources.Load<Camera>();
	scene.UseCamera(first);
	scene.UseCamera(second);

	ASSERT_TRUE(scene.ActivateCamera(second->GetObjectID()));
	EXPECT_EQ(scene.ActiveCameraID(), second->GetObjectID());
	EXPECT_EQ(scene.GetActiveCamera(), second.get());

	// Replacing, not accumulating: the previous activation is simply dropped.
	ASSERT_TRUE(scene.ActivateCamera(first->GetObjectID()));
	EXPECT_EQ(scene.GetActiveCamera(), first.get());
}

TEST(SceneCameraActivation, DeactivateCamera_LeavesNoSceneCameraInUse)
{
	Scene scene;
	ResourceManager resources;

	auto cam = resources.Load<Camera>();
	scene.UseCamera(cam);
	ASSERT_TRUE(scene.ActivateCamera(cam->GetObjectID()));

	scene.DeactivateCamera();
	EXPECT_EQ(scene.ActiveCameraID(), 0);
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);
	// The camera is still scene content — deactivating is not deleting.
	EXPECT_EQ(scene.cam_list.size(), 1u);
}

// ---------------------------------------------------------------------------
// Only scene content can be activated
// ---------------------------------------------------------------------------

/**
 * @test A pooled camera that was never handed to UseCamera cannot be activated.
 *       This is exactly the editor camera's situation — it lives in the
 *       ResourceManager so CameraController can resolve and orbit it, but it is
 *       not scene content and must not be reachable as a *scene* activation.
 */
TEST(SceneCameraActivation, ActivateCamera_RejectsAPooledNonSceneCamera)
{
	Scene scene;
	ResourceManager resources;

	auto sceneCam = resources.Load<Camera>();
	scene.UseCamera(sceneCam);
	ASSERT_TRUE(scene.ActivateCamera(sceneCam->GetObjectID()));

	auto editorCam = resources.Load<Camera>(); // pooled, never registered
	EXPECT_FALSE(scene.ActivateCamera(editorCam->GetObjectID()));

	// A refused activation changes nothing.
	EXPECT_EQ(scene.ActiveCameraID(), sceneCam->GetObjectID());
	EXPECT_EQ(scene.GetActiveCamera(), sceneCam.get());
}

TEST(SceneCameraActivation, ActivateCamera_RejectsAnUnknownUid)
{
	Scene scene;
	ResourceManager resources;
	scene.UseCamera(resources.Load<Camera>());

	EXPECT_FALSE(scene.ActivateCamera(0));
	EXPECT_FALSE(scene.ActivateCamera(999999));
	EXPECT_EQ(scene.ActiveCameraID(), 0);
}

// ---------------------------------------------------------------------------
// Removal leaves the UID stale on purpose, so undo restores it for free
// ---------------------------------------------------------------------------

/**
 * @test Deleting the activated camera is legal and takes the view with it: the
 *       scene reports no camera in use, so the Editor falls back to its own.
 *       ActiveCameraID() keeps naming the removed camera — that staleness is the
 *       feature, not a leak, and the next test is why.
 */
TEST(SceneCameraActivation, RemovingTheActivatedCamera_LeavesTheUidStale)
{
	Scene scene;
	ResourceManager resources;

	auto cam = resources.Load<Camera>();
	const int uid = cam->GetObjectID();
	scene.UseCamera(cam);
	ASSERT_TRUE(scene.ActivateCamera(uid));

	ASSERT_TRUE(scene.RemoveCamera(uid));
	EXPECT_TRUE(scene.cam_list.empty());
	EXPECT_EQ(scene.GetActiveCamera(), nullptr) << "must validate against cam_list";
	EXPECT_EQ(scene.ActiveCameraID(), uid) << "left stale so undo can revalidate it";
}

/**
 * @test Undoing the delete re-validates the activation with no extra operation.
 *       The undo path is SceneObjectAddOp -> UsePooledObject -> UseCamera, which
 *       is what this stands in for; because the UID was never cleared, putting
 *       the camera back in cam_list is enough to restore the view, which keeps
 *       delete-and-undo at exactly one undo entry each way.
 */
TEST(SceneCameraActivation, ReAddingTheRemovedCamera_RestoresTheActivation)
{
	Scene scene;
	ResourceManager resources;

	auto cam = resources.Load<Camera>();
	const int uid = cam->GetObjectID();
	scene.UseCamera(cam);
	ASSERT_TRUE(scene.ActivateCamera(uid));
	ASSERT_TRUE(scene.RemoveCamera(uid));
	ASSERT_EQ(scene.GetActiveCamera(), nullptr);

	scene.UseCamera(cam); // the undo of the delete
	EXPECT_EQ(scene.GetActiveCamera(), cam.get())
		<< "the surviving UID must re-resolve without a second activation";
}

TEST(SceneCameraActivation, RemovingADifferentCamera_DoesNotDisturbTheActivation)
{
	Scene scene;
	ResourceManager resources;

	auto kept = resources.Load<Camera>();
	auto other = resources.Load<Camera>();
	scene.UseCamera(kept);
	scene.UseCamera(other);
	ASSERT_TRUE(scene.ActivateCamera(kept->GetObjectID()));

	ASSERT_TRUE(scene.RemoveCamera(other->GetObjectID()));
	EXPECT_EQ(scene.GetActiveCamera(), kept.get());
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

/**
 * @test A scene saved with nothing activated loads with nothing activated, even
 *       though it contains cameras. "Explicitly none" and "field absent" are
 *       different states and only the latter migrates — otherwise every
 *       camera-bearing scene would silently reacquire a scene camera on reload
 *       and the editor camera would be unreachable in practice.
 */
TEST(SceneCameraActivation, ExplicitNoneRoundTripsAsNoneAndNeverMigrates)
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

/**
 * @test A project written before the field existed migrates once, activating the
 *       camera the old positional lookup would have returned, so an upgraded
 *       project still opens through the camera it was saved with. The file is a
 *       real save with the field surgically removed — see StripActiveCamUid.
 */
TEST(SceneCameraActivation, LegacyFileWithoutTheFieldMigratesToACamera)
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
	// Which one is hash order's business, as it was before the feature existed;
	// what matters is that the migration leaves a usable camera in use rather
	// than silently blanking a project that used to render.
	EXPECT_NE(loaded.ActiveCameraID(), 0);
	Camera* cam = loaded.GetActiveCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->GetObjectID(), loaded.ActiveCameraID());
	EXPECT_EQ(cam->GetObjectID(), loaded.cam_list.begin()->first);
}

/**
 * @test A legacy file with no cameras at all migrates to "none" rather than to a
 *       conjured camera. Nothing is injected to repair a camera-less document:
 *       zero cameras is legal, because the Editor always has its own.
 */
TEST(SceneCameraActivation, LegacyCameraLessFileMigratesToNone)
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
