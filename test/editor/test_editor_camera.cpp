/**
 * @file test_editor_camera.cpp
 * @brief The editor camera: the Editor's own view camera, which is what makes a
 *        camera-less scene a legal document.
 *
 * The viewport used to look through whatever Scene::GetActiveCamera() returned,
 * so a scene had to contain a camera or four passes silently skipped their work,
 * and orbiting to inspect a model permanently edited scene content. The Editor
 * now owns a camera of its own: pooled (so CameraController can resolve and
 * orbit it by UID like any other camera) but never scene content (so it cannot
 * be selected, deleted, or activated through the Scene).
 *
 * Three properties are load-bearing and pinned here:
 *
 * - It is re-established after every ResourceManager::Clear(). Both scene-swap
 *   paths clear the pool, so the camera cannot be created once in the
 *   constructor; EnsureEditorCamera() runs on each of them.
 * - Its identity survives save/load, via EditorComponent recording the UID while
 *   ResourceComponent serializes the camera object with the rest of the pool.
 * - An activated scene camera takes the view, and deactivating hands it back to
 *   the editor camera *at the pose it was left at* — the editor camera is not
 *   reset by a scene camera coming and going.
 *
 * No GPU and no Vulkan: `Editor(nullptr, nullptr)` skips every upload path. The
 * starter assets are CPU-side loads resolved against the CWD, which CTest pins
 * to the build dir (res/ is copied there by a POST_BUILD step).
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "asset/Project.h"
#include "asset/components/EditorComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/SceneComponent.h"
#include "core/ResourceManager.h"
#include "editor/Editor.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

// Force-link the polymorphic registration TUs (static libs).
#include "asset/registrations/DataRegistration.h"
#include "scene/registrations/TypeRegistration.h"

using namespace neurus;

namespace
{

/// Same starter mesh Application passes to NewScene() / CreateDefaultScene().
constexpr const char* kObj = "res/obj/sphere.obj";

struct TempFile
{
	std::string path;
	explicit TempFile(std::string p) : path(std::move(p)) {}
	~TempFile() { std::remove(path.c_str()); }
};

/**
 * @brief Registers the components this file needs, in Application's order.
 *
 * Pool first (the Scene resolves its ID references against it), EditorComponent
 * last (registration order is archive read order, and it is the appended-last
 * node). Config/UI/History are omitted; what matters is that save and load use
 * the same set.
 */
void BuildProject(project::Project& proj, Editor& editor)
{
	proj.Register<project::ResourceComponent>(editor.GetResourceManager());
	proj.Register<project::SceneComponent>(editor.GetScene(),
	                                       editor.GetResourceManager());
	proj.Register<project::EditorComponent>(editor);
}

void SaveProject(const std::string& path, Editor& editor)
{
	project::Project proj;
	BuildProject(proj, editor);
	proj.Save(path);
}

/// @brief The Application::OnProjectOpen sequence, minus the UI layout.
void LoadProject(const std::string& path, Editor& editor)
{
	editor.BeginLoad();
	project::Project proj;
	BuildProject(proj, editor);
	proj.Load(path);
	editor.FinishLoad();
}

} // namespace

// ---------------------------------------------------------------------------
// Identity: pooled, but not scene content
// ---------------------------------------------------------------------------

/**
 * @test The editor camera is the view camera, lives in the pool, and is absent
 *       from the scene. Being pooled is what lets CameraController resolve it by
 *       UID (it looks cameras up in the resource pool, not in cam_list) so the
 *       ordinary orbit/zoom/resize events reach it; being outside cam_list is
 *       what keeps it out of the Outliner and unactivatable.
 */
TEST(EditorCameraTest, EditorCameraIsPooledButNotSceneContent)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	const int uid = editor.EditorCameraID();
	ASSERT_NE(uid, 0);

	const Camera* view = editor.ViewCamera();
	ASSERT_NE(view, nullptr);
	EXPECT_EQ(view->GetObjectID(), uid);

	// In the pool...
	EXPECT_NE(editor.GetResourceManager().Get<Camera>(uid), nullptr);
	// ...but not in the scene, under any of its lookups.
	Scene& scene = editor.GetScene();
	EXPECT_EQ(scene.cam_list.count(uid), 0u);
	EXPECT_EQ(scene.GetObjectID(uid), nullptr);
	EXPECT_FALSE(scene.ActivateCamera(uid)) << "the Scene must refuse to activate it";
}

/**
 * @test The default framing is the one the Editor seeds, not a default-constructed
 *       Camera. A fresh Camera is degenerate for viewing (eye == target), so the
 *       pose has to be set explicitly.
 */
TEST(EditorCameraTest, EditorCameraIsSeededWithAUsablePose)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	const Camera* cam = editor.ViewCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->GetPosition(), glm::vec3(0.0f, -5.0f, 2.0f));
	EXPECT_EQ(cam->cam_tar, glm::vec3(0.0f));
	EXPECT_NE(cam->GetPosition(), cam->cam_tar);
}

// ---------------------------------------------------------------------------
// Lifecycle: re-established after every pool clear
// ---------------------------------------------------------------------------

/**
 * @test Repeated File > New keeps a usable editor camera. Each scene swap calls
 *       ResourceManager::Clear(), which drops the editor camera along with
 *       everything else — so it gets a *new* UID each time, and a stale UID left
 *       behind would resolve to nothing and blank the viewport.
 */
TEST(EditorCameraTest, RepeatedNewScene_RebuildsTheEditorCamera)
{
	Editor editor(nullptr, nullptr);

	int previousUid = 0;
	for (int i = 0; i < 3; ++i)
	{
		editor.NewScene(kObj);

		const int uid = editor.EditorCameraID();
		ASSERT_NE(uid, 0) << "iteration " << i;
		EXPECT_NE(uid, previousUid) << "the pool was cleared; the UID cannot survive";
		previousUid = uid;

		const Camera* cam = editor.ViewCamera();
		ASSERT_NE(cam, nullptr) << "iteration " << i;
		EXPECT_EQ(cam->GetObjectID(), uid);
		// Reset to the seeded framing, not carried over from the previous document.
		EXPECT_EQ(cam->GetPosition(), glm::vec3(0.0f, -5.0f, 2.0f));
	}
}

/**
 * @test Deleting every camera in the scene is legal and leaves the viewport
 *       working. This used to be refused outright by SceneController ("Refusing
 *       to delete the last camera") because the view depended on scene content.
 */
TEST(EditorCameraTest, DeletingEverySceneCamera_LeavesAWorkingViewCamera)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	Scene& scene = editor.GetScene();
	ASSERT_FALSE(scene.cam_list.empty());
	while (!scene.cam_list.empty())
		ASSERT_TRUE(scene.RemoveCamera(scene.cam_list.begin()->first));

	EXPECT_EQ(scene.GetActiveCamera(), nullptr);
	const Camera* cam = editor.ViewCamera();
	ASSERT_NE(cam, nullptr) << "a camera-less scene must still be renderable";
	EXPECT_EQ(cam->GetObjectID(), editor.EditorCameraID());
}

// ---------------------------------------------------------------------------
// Activation switches the view; deactivation hands it back unchanged
// ---------------------------------------------------------------------------

/**
 * @test Activating a scene camera moves the view to it, and deactivating returns
 *       to the editor camera *at the pose it was left at*. The editor camera is
 *       not reset, re-framed or reseeded by a scene camera coming and going — the
 *       user's viewpoint has to be where they left it.
 */
TEST(EditorCameraTest, ActivationSwitchesTheViewAndDeactivationRestoresThePose)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	// Stand in for the user having orbited: a pose the seeded default is not.
	Camera* editorCam = editor.ViewCamera();
	ASSERT_NE(editorCam, nullptr);
	const glm::vec3 orbited(3.0f, -7.0f, 4.0f);
	editorCam->SetPosition(orbited);

	Scene& scene = editor.GetScene();
	ASSERT_EQ(scene.cam_list.size(), 1u);
	Camera* sceneCam = scene.cam_list.begin()->second.get();
	ASSERT_TRUE(scene.ActivateCamera(sceneCam->GetObjectID()));

	EXPECT_EQ(editor.ViewCamera(), sceneCam) << "an activated scene camera takes the view";
	EXPECT_NE(editor.ViewCamera()->GetObjectID(), editor.EditorCameraID());

	scene.DeactivateCamera();
	ASSERT_EQ(editor.ViewCamera(), editorCam);
	EXPECT_EQ(editor.ViewCamera()->GetPosition(), orbited)
		<< "the editor camera must keep the pose it was left at";
}

/**
 * @test Selecting a scene camera does not change the view. Selection drives the
 *       PropertyPanel; only activation drives the viewport.
 */
TEST(EditorCameraTest, SelectingASceneCameraDoesNotChangeTheView)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	Scene& scene = editor.GetScene();
	ASSERT_EQ(scene.cam_list.size(), 1u);
	scene.selections.Select(scene.cam_list.begin()->second.get(), false);

	ASSERT_NE(editor.ViewCamera(), nullptr);
	EXPECT_EQ(editor.ViewCamera()->GetObjectID(), editor.EditorCameraID());
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

/**
 * @test The editor camera's identity AND pose survive a save/reopen. The object
 *       rides along in the serialized pool; EditorComponent records only which
 *       pooled camera the Editor claims as its own, which is the part that would
 *       otherwise be lost — FinishLoad() would then mint a fresh camera at the
 *       default framing and the user's viewpoint would snap back on every open.
 */
TEST(EditorCameraTest, EditorCameraSurvivesSaveAndReopen)
{
	TempFile tmp("test_editor_camera_rt.neurus.json");
	const glm::vec3 pose(2.0f, -6.0f, 3.0f);
	int savedUid = 0;

	{
		Editor editor(nullptr, nullptr);
		editor.NewScene(kObj);
		ASSERT_NE(editor.ViewCamera(), nullptr);
		editor.ViewCamera()->SetPosition(pose); // as if the user had orbited
		savedUid = editor.EditorCameraID();
		SaveProject(tmp.path, editor);
	}

	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj); // a document is already open when File > Open runs
	ASSERT_NE(editor.EditorCameraID(), savedUid) << "precondition: a different camera";
	LoadProject(tmp.path, editor);

	EXPECT_EQ(editor.EditorCameraID(), savedUid);
	const Camera* cam = editor.ViewCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->GetObjectID(), savedUid);
	EXPECT_EQ(cam->GetPosition(), pose);
}

/**
 * @test A project saved before EditorComponent existed still opens, with a fresh
 *       editor camera minted at the default framing. Simulated by saving without
 *       that component registered, which is exactly what such a file looks like:
 *       the node is simply absent and Load() falls back instead of throwing.
 */
TEST(EditorCameraTest, ProjectWithoutTheSavedUid_MintsAFreshEditorCamera)
{
	TempFile tmp("test_editor_camera_legacy.neurus.json");

	{
		Editor editor(nullptr, nullptr);
		editor.NewScene(kObj);
		project::Project proj;
		proj.Register<project::ResourceComponent>(editor.GetResourceManager());
		proj.Register<project::SceneComponent>(editor.GetScene(),
		                                       editor.GetResourceManager());
		proj.Save(tmp.path); // no EditorComponent: the node never gets written
	}

	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);
	LoadProject(tmp.path, editor);

	EXPECT_NE(editor.EditorCameraID(), 0);
	const Camera* cam = editor.ViewCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->GetObjectID(), editor.EditorCameraID());
	EXPECT_EQ(cam->GetPosition(), glm::vec3(0.0f, -5.0f, 2.0f));
}

/**
 * @test A project whose scene has no camera at all opens, and the viewport still
 *       has something to look through. This is the case the loader used to
 *       "repair" by silently injecting a default camera into the Outliner; zero
 *       scene cameras is now a legal document because the Editor always has its
 *       own.
 */
TEST(EditorCameraTest, CameraLessProjectLoads_WithNothingInjected)
{
	TempFile tmp("test_editor_camera_nocam.neurus.json");

	{
		Editor editor(nullptr, nullptr);
		editor.NewScene(kObj);
		Scene& scene = editor.GetScene();
		while (!scene.cam_list.empty())
			ASSERT_TRUE(scene.RemoveCamera(scene.cam_list.begin()->first));
		SaveProject(tmp.path, editor);
	}

	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);
	ASSERT_FALSE(editor.GetScene().cam_list.empty()) << "precondition: the starter camera";
	LoadProject(tmp.path, editor);

	Scene& scene = editor.GetScene();
	EXPECT_TRUE(scene.cam_list.empty()) << "nothing may be injected to repair the document";
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);
	EXPECT_EQ(scene.ActiveCameraID(), 0);

	const Camera* cam = editor.ViewCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->GetObjectID(), editor.EditorCameraID());
	EXPECT_EQ(scene.mesh_list.size(), 1u) << "the rest of the document must still load";
}

// ---------------------------------------------------------------------------
// Aspect ratio reaches both view candidates
// ---------------------------------------------------------------------------

/**
 * @test One resize re-frames the editor camera *and* the activated scene camera,
 *       not just whichever is currently in use. A default-constructed Camera is
 *       1x1, and activation can switch the view with no resize in between — so a
 *       camera that missed a resize would present a stretched image the moment it
 *       took the view. Both events are enqueued by HandleResize and resolved
 *       through the pool by CameraController, which is why Edit() drains them.
 */
TEST(EditorCameraTest, ResizeReframesBothTheEditorAndTheActivatedSceneCamera)
{
	Editor editor(nullptr, nullptr);
	editor.Initialize(); // registers CameraController, which owns CameraResizeEvent
	editor.NewScene(kObj);

	Scene& scene = editor.GetScene();
	ASSERT_EQ(scene.cam_list.size(), 1u);
	Camera* sceneCam = scene.cam_list.begin()->second.get();
	ASSERT_TRUE(scene.ActivateCamera(sceneCam->GetObjectID()));

	Camera* editorCam = editor.GetResourceManager().Get<Camera>(editor.EditorCameraID()).get();
	ASSERT_NE(editorCam, nullptr);
	ASSERT_NE(editorCam->cam_w, 800.0f) << "precondition: no viewport extent yet";

	editor.HandleResize(glm::uvec2(800, 600), glm::uvec2(800, 600));
	editor.Edit(); // drains both CameraResizeEvents

	EXPECT_FLOAT_EQ(sceneCam->cam_w, 800.0f);
	EXPECT_FLOAT_EQ(sceneCam->cam_h, 600.0f);
	EXPECT_FLOAT_EQ(editorCam->cam_w, 800.0f) << "the camera not in use must be re-framed too";
	EXPECT_FLOAT_EQ(editorCam->cam_h, 600.0f);
}
