/**
 * @file test_viewport_camera.cpp
 * @brief The viewport camera: the EditorViewport's own view camera, which is what
 *        makes a camera-less scene a legal document.
 *
 * The viewport used to look through whatever Scene::GetActiveCamera() returned,
 * so a scene had to contain a camera or four passes silently skipped their work,
 * and orbiting to inspect a model permanently edited scene content. The
 * EditorViewport now owns a camera of its own: pooled (so CameraController can
 * resolve and orbit it by UID like any other camera) but never scene content (so
 * it cannot be selected, deleted, or activated through the Scene).
 *
 * The Editor itself has no camera API left — not an accessor, not a resize entry
 * point. Every test here therefore reaches through `editor.GetViewport()`, and
 * that is the property under test as much as any assertion below: the only object
 * that knows a viewport camera exists is the viewport.
 *
 * Four things are load-bearing and pinned here:
 *
 * - The camera is re-established after every ResourceManager::Clear(). Both
 *   scene-swap paths clear the pool, so it cannot be created once in a
 *   constructor; EditorViewport::EnsureCamera() runs on each of them.
 * - Its identity survives save/load, via EditorComponent recording the UID while
 *   ResourceComponent serializes the camera object with the rest of the pool —
 *   including from a file written under the pre-rename `editorCamUid` key.
 * - An activated scene camera takes the view, and deactivating hands it back to
 *   the viewport camera *at the pose it was left at*: the viewport camera is not
 *   reset by a scene camera coming and going.
 * - Which camera a frame renders through is *published* by SyncCamera(), not
 *   recomputed per query, so a test that toggles activation by hand has to say
 *   where the frame boundary is (SyncFrame() below).
 *
 * No GPU and no Vulkan: `Editor(nullptr)` skips every upload path. The
 * starter assets are CPU-side loads resolved against the CWD, which CTest pins
 * to the build dir (res/ is copied there by a POST_BUILD step).
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "asset/Project.h"
#include "asset/components/EditorComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/SceneComponent.h"
#include "core/ResourceManager.h"
#include "editor/Editor.h"
#include "editor/viewport/EditorViewport.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

// Force-link the polymorphic registration TUs (static libs).
#include "asset/registrations/DataRegistration.h"
#include "scene/registrations/TypeRegistration.h"

using namespace neurus;

namespace
{

/// Same starter mesh Application passes to CreateDefaultScene().
constexpr const char* kObj = "res/obj/sphere.obj";

/**
 * @brief The two steps "File > New" means, in the Application's order.
 *
 * `NewScene()` is an empty document (no mesh, no light, no scene camera) and
 * `CreateDefaultScene()` is the starter content put into it; the Application
 * composes them (OnProjectNew). Tests about the viewport camera *versus* scene
 * cameras need the content; tests about the viewport camera alone call
 * `NewScene()` directly.
 */
void NewProject(Editor& editor)
{
	editor.NewScene();
	editor.CreateDefaultScene(kObj);
}

/**
 * @brief Re-publishes the view camera, exactly where a frame would.
 *
 * `EditorViewport::GetCamera()` hands back a pointer the viewport *published*,
 * not a choice it recomputes per call — one pointer per frame is what stops the
 * gizmo's projection math and the renderer's CameraGPU from disagreeing about
 * which camera the frame belongs to. Activation is a Scene mutation, so a test
 * that activates or deactivates by hand has to name the frame boundary itself;
 * Editor::Edit() is the production caller.
 */
void SyncFrame(Editor& editor)
{
	editor.GetViewport().SyncCamera(editor.GetScene());
}

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
 * the same set. Note what EditorComponent binds to: the viewport, not the Editor.
 */
void BuildProject(project::Project& proj, Editor& editor)
{
	proj.Register<project::ResourceComponent>(editor.GetResourceManager());
	proj.Register<project::SceneComponent>(editor.GetScene(),
	                                       editor.GetResourceManager());
	proj.Register<project::EditorComponent>(editor.GetViewport());
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

/**
 * @brief Rewrites every occurrence of @p from to @p to in a saved project file.
 *
 * How a file from an older build is produced without keeping a fixture around:
 * save normally, then rename the one key that was renamed. Cheaper and far more
 * honest than hand-writing a JSON blob that would drift from the real format.
 */
void RewriteKey(const std::string& path, const std::string& from, const std::string& to)
{
	std::string text;
	{
		std::ifstream in(path);
		std::ostringstream buf;
		buf << in.rdbuf();
		text = buf.str();
	}

	for (std::size_t at = text.find(from); at != std::string::npos;
	     at = text.find(from, at + to.size()))
		text.replace(at, from.size(), to);

	std::ofstream out(path, std::ios::trunc);
	out << text;
}

} // namespace

// ---------------------------------------------------------------------------
// Identity: pooled, but not scene content
// ---------------------------------------------------------------------------

/**
 * @test The viewport camera is the view camera, lives in the pool, and is absent
 *       from the scene. Being pooled is what lets CameraController resolve it by
 *       UID (it looks cameras up in the resource pool, not in cam_list) so the
 *       ordinary orbit/zoom events reach it; being outside cam_list is what keeps
 *       it out of the Outliner and unactivatable.
 */
TEST(ViewportCameraTest, ViewportCameraIsPooledButNotSceneContent)
{
	Editor editor(nullptr);
	NewProject(editor);

	const EditorViewport& vp = editor.GetViewport();
	const int uid = vp.CameraID();
	ASSERT_NE(uid, 0);

	const Camera* view = vp.GetCamera();
	ASSERT_NE(view, nullptr);
	EXPECT_EQ(view, vp.ViewportCamera()) << "no scene camera is activated";
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
 * @test The default framing is the one the viewport seeds, not a
 *       default-constructed Camera. A fresh Camera is degenerate for viewing
 *       (eye == target), so the pose has to be set explicitly — and the two
 *       constants live on EditorViewport, which is the only definition of them.
 */
TEST(ViewportCameraTest, ViewportCameraIsSeededWithAUsablePose)
{
	Editor editor(nullptr);
	editor.NewScene(); // no content needed: this is the viewport camera alone

	const Camera* cam = editor.GetViewport().ViewportCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->GetPosition(), EditorViewport::kDefaultEye);
	EXPECT_EQ(cam->cam_tar, EditorViewport::kDefaultTarget);
	EXPECT_NE(cam->GetPosition(), cam->cam_tar);
}

// ---------------------------------------------------------------------------
// Lifecycle: re-established after every pool clear
// ---------------------------------------------------------------------------

/**
 * @test Repeated File > New keeps a usable viewport camera. Each scene swap calls
 *       ResourceManager::Clear(), which drops the viewport camera along with
 *       everything else — so it gets a *new* UID each time, and a stale UID left
 *       behind would resolve to nothing and blank the viewport. ForgetCamera()
 *       before the clear is what makes that true.
 */
TEST(ViewportCameraTest, RepeatedNewScene_RebuildsTheViewportCamera)
{
	Editor editor(nullptr);

	int previousUid = 0;
	for (int i = 0; i < 3; ++i)
	{
		editor.NewScene(); // the reset is NewScene()'s half of File > New

		const EditorViewport& vp = editor.GetViewport();
		const int uid = vp.CameraID();
		ASSERT_NE(uid, 0) << "iteration " << i;
		EXPECT_NE(uid, previousUid) << "the pool was cleared; the UID cannot survive";
		previousUid = uid;

		const Camera* cam = vp.GetCamera();
		ASSERT_NE(cam, nullptr) << "iteration " << i;
		EXPECT_EQ(cam->GetObjectID(), uid);
		// Reset to the seeded framing, not carried over from the previous document.
		EXPECT_EQ(cam->GetPosition(), EditorViewport::kDefaultEye);
	}
}

/**
 * @test Deleting every camera in the scene is legal and leaves the viewport
 *       working. This used to be refused outright by SceneController ("Refusing
 *       to delete the last camera") because the view depended on scene content.
 */
TEST(ViewportCameraTest, DeletingEverySceneCamera_LeavesAWorkingViewCamera)
{
	Editor editor(nullptr);
	NewProject(editor);

	Scene& scene = editor.GetScene();
	ASSERT_FALSE(scene.cam_list.empty());
	while (!scene.cam_list.empty())
		ASSERT_TRUE(scene.RemoveCamera(scene.cam_list.begin()->first));

	EXPECT_EQ(scene.GetActiveCamera(), nullptr);
	SyncFrame(editor);

	const EditorViewport& vp = editor.GetViewport();
	const Camera* cam = vp.GetCamera();
	ASSERT_NE(cam, nullptr) << "a camera-less scene must still be renderable";
	EXPECT_EQ(cam->GetObjectID(), vp.CameraID());
}

// ---------------------------------------------------------------------------
// Activation switches the view; deactivation hands it back unchanged
// ---------------------------------------------------------------------------

/**
 * @test Activating a scene camera moves the view to it, and deactivating returns
 *       to the viewport camera *at the pose it was left at*. The viewport camera
 *       is not reset, re-framed or reseeded by a scene camera coming and going —
 *       the user's viewpoint has to be where they left it.
 */
TEST(ViewportCameraTest, ActivationSwitchesTheViewAndDeactivationRestoresThePose)
{
	Editor editor(nullptr);
	NewProject(editor);

	// Stand in for the user having orbited: a pose the seeded default is not.
	Camera* viewportCam = editor.GetViewport().ViewportCamera();
	ASSERT_NE(viewportCam, nullptr);
	const glm::vec3 orbited(3.0f, -7.0f, 4.0f);
	viewportCam->SetPosition(orbited);

	Scene& scene = editor.GetScene();
	ASSERT_EQ(scene.cam_list.size(), 1u);
	Camera* sceneCam = scene.cam_list.begin()->second.get();
	ASSERT_TRUE(scene.ActivateCamera(sceneCam->GetObjectID()));
	SyncFrame(editor);

	const EditorViewport& vp = editor.GetViewport();
	EXPECT_EQ(vp.GetCamera(), sceneCam) << "an activated scene camera takes the view";
	EXPECT_NE(vp.GetCamera()->GetObjectID(), vp.CameraID());

	scene.DeactivateCamera();
	SyncFrame(editor);
	ASSERT_EQ(vp.GetCamera(), viewportCam);
	EXPECT_EQ(vp.GetCamera()->GetPosition(), orbited)
		<< "the viewport camera must keep the pose it was left at";
}

/**
 * @test Selecting a scene camera does not change the view. Selection drives the
 *       PropertyPanel; only activation drives the viewport.
 */
TEST(ViewportCameraTest, SelectingASceneCameraDoesNotChangeTheView)
{
	Editor editor(nullptr);
	NewProject(editor);

	Scene& scene = editor.GetScene();
	ASSERT_EQ(scene.cam_list.size(), 1u);
	scene.selections.Select(scene.cam_list.begin()->second.get(), false);
	SyncFrame(editor);

	const EditorViewport& vp = editor.GetViewport();
	ASSERT_NE(vp.GetCamera(), nullptr);
	EXPECT_EQ(vp.GetCamera()->GetObjectID(), vp.CameraID());
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

/**
 * @test The viewport camera's identity AND pose survive a save/reopen. The object
 *       rides along in the serialized pool; EditorComponent records only which
 *       pooled camera the viewport claims as its own, which is the part that would
 *       otherwise be lost — FinishLoad() would then mint a fresh camera at the
 *       default framing and the user's viewpoint would snap back on every open.
 */
TEST(ViewportCameraTest, ViewportCameraSurvivesSaveAndReopen)
{
	TempFile tmp("test_viewport_camera_rt.neurus.json");
	const glm::vec3 pose(2.0f, -6.0f, 3.0f);
	int savedUid = 0;

	{
		Editor editor(nullptr);
		NewProject(editor);
		ASSERT_NE(editor.GetViewport().ViewportCamera(), nullptr);
		editor.GetViewport().ViewportCamera()->SetPosition(pose); // as if orbited
		savedUid = editor.GetViewport().CameraID();
		SaveProject(tmp.path, editor);
	}

	Editor editor(nullptr);
	NewProject(editor); // a document is already open when File > Open runs
	ASSERT_NE(editor.GetViewport().CameraID(), savedUid)
		<< "precondition: a different camera";
	LoadProject(tmp.path, editor);

	const EditorViewport& vp = editor.GetViewport();
	EXPECT_EQ(vp.CameraID(), savedUid);
	const Camera* cam = vp.GetCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->GetObjectID(), savedUid);
	EXPECT_EQ(cam->GetPosition(), pose);
}

/**
 * @test A project written under the pre-rename key still restores the saved
 *       camera. The field moved from `editorCamUid` to `viewportCamUid` when the
 *       camera moved from the Editor into the EditorViewport, and the id on either
 *       side names the same pooled object — so this is a migration, not a default:
 *       falling back to a fresh camera here would silently snap the user's
 *       viewpoint back to the seeded pose the first time they opened an older file.
 */
TEST(ViewportCameraTest, LegacyEditorCamUidKey_StillRestoresTheSavedCamera)
{
	TempFile tmp("test_viewport_camera_legacykey.neurus.json");
	const glm::vec3 pose(-4.0f, -2.0f, 5.0f);
	int savedUid = 0;

	{
		Editor editor(nullptr);
		NewProject(editor);
		editor.GetViewport().ViewportCamera()->SetPosition(pose);
		savedUid = editor.GetViewport().CameraID();
		SaveProject(tmp.path, editor);
	}

	// Exactly what a file from before the rename contains.
	RewriteKey(tmp.path, "viewportCamUid", "editorCamUid");

	Editor editor(nullptr);
	NewProject(editor);
	ASSERT_NE(editor.GetViewport().CameraID(), savedUid);
	LoadProject(tmp.path, editor);

	const EditorViewport& vp = editor.GetViewport();
	EXPECT_EQ(vp.CameraID(), savedUid) << "the legacy key must still be read";
	ASSERT_NE(vp.GetCamera(), nullptr);
	EXPECT_EQ(vp.GetCamera()->GetPosition(), pose);
}

/**
 * @test A project saved before EditorComponent existed still opens, with a fresh
 *       viewport camera minted at the default framing. Simulated by saving without
 *       that component registered, which is exactly what such a file looks like:
 *       the node is simply absent and Load() falls back instead of throwing.
 */
TEST(ViewportCameraTest, ProjectWithoutTheSavedUid_MintsAFreshViewportCamera)
{
	TempFile tmp("test_viewport_camera_legacy.neurus.json");

	{
		Editor editor(nullptr);
		NewProject(editor);
		project::Project proj;
		proj.Register<project::ResourceComponent>(editor.GetResourceManager());
		proj.Register<project::SceneComponent>(editor.GetScene(),
		                                       editor.GetResourceManager());
		proj.Save(tmp.path); // no EditorComponent: the node never gets written
	}

	Editor editor(nullptr);
	NewProject(editor);
	LoadProject(tmp.path, editor);

	const EditorViewport& vp = editor.GetViewport();
	EXPECT_NE(vp.CameraID(), 0);
	const Camera* cam = vp.GetCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->GetObjectID(), vp.CameraID());
	EXPECT_EQ(cam->GetPosition(), EditorViewport::kDefaultEye);
}

/**
 * @test A project whose scene has no camera at all opens, and the viewport still
 *       has something to look through. This is the case the loader used to
 *       "repair" by silently injecting a default camera into the Outliner; zero
 *       scene cameras is now a legal document because the viewport always has its
 *       own.
 */
TEST(ViewportCameraTest, CameraLessProjectLoads_WithNothingInjected)
{
	TempFile tmp("test_viewport_camera_nocam.neurus.json");

	{
		Editor editor(nullptr);
		NewProject(editor);
		Scene& scene = editor.GetScene();
		while (!scene.cam_list.empty())
			ASSERT_TRUE(scene.RemoveCamera(scene.cam_list.begin()->first));
		SaveProject(tmp.path, editor);
	}

	Editor editor(nullptr);
	NewProject(editor);
	ASSERT_FALSE(editor.GetScene().cam_list.empty()) << "precondition: the starter camera";
	LoadProject(tmp.path, editor);

	Scene& scene = editor.GetScene();
	EXPECT_TRUE(scene.cam_list.empty()) << "nothing may be injected to repair the document";
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);
	EXPECT_EQ(scene.ActiveCameraID(), 0);

	const EditorViewport& vp = editor.GetViewport();
	const Camera* cam = vp.GetCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_EQ(cam->GetObjectID(), vp.CameraID());
	EXPECT_EQ(scene.mesh_list.size(), 1u) << "the rest of the document must still load";
}

// ---------------------------------------------------------------------------
// Aspect ratio reaches both view candidates
// ---------------------------------------------------------------------------

/**
 * @test One resize re-frames the viewport camera *and* the activated scene camera,
 *       not just whichever is currently in use. A default-constructed Camera is
 *       1x1, and activation can switch the view with no resize in between — so a
 *       camera that missed a resize would present a stretched image the moment it
 *       took the view.
 *
 *       The two are framed at different *moments*, which is the design and not an
 *       accident: HandleResize() frames the viewport's own camera immediately,
 *       because a resize must be in effect for the frame that follows it, while
 *       the activated scene camera needs a Scene to find and is picked up by the
 *       next SyncCamera() — which Edit() runs before anything draws. No
 *       CameraResizeEvent is involved any more: there is one framing mechanism
 *       (EditorViewport::FrameCamera) rather than an event path beside it.
 */
TEST(ViewportCameraTest, ResizeReframesBothTheViewportAndTheActivatedSceneCamera)
{
	Editor editor(nullptr);
	NewProject(editor);

	Scene& scene = editor.GetScene();
	ASSERT_EQ(scene.cam_list.size(), 1u);
	Camera* sceneCam = scene.cam_list.begin()->second.get();
	ASSERT_TRUE(scene.ActivateCamera(sceneCam->GetObjectID()));

	Camera* viewportCam = editor.GetViewport().ViewportCamera();
	ASSERT_NE(viewportCam, nullptr);
	ASSERT_NE(viewportCam->cam_w, 800.0f) << "precondition: no viewport extent yet";

	editor.GetViewport().HandleResize(glm::uvec2(800, 600), glm::uvec2(800, 600));

	EXPECT_FLOAT_EQ(viewportCam->cam_w, 800.0f)
		<< "the camera not in use must be re-framed too, and immediately";
	EXPECT_FLOAT_EQ(viewportCam->cam_h, 600.0f);

	editor.Edit(); // SyncCamera() reaches the activated scene camera

	EXPECT_FLOAT_EQ(sceneCam->cam_w, 800.0f);
	EXPECT_FLOAT_EQ(sceneCam->cam_h, 600.0f);
}

/**
 * @test A zero extent is dropped rather than stored. It is what the window reports
 *       before it is first shown, and a zero height would divide by zero in the
 *       projection — the previous extent is a better answer than none.
 */
TEST(ViewportCameraTest, ZeroResizeIsIgnored)
{
	Editor editor(nullptr);
	editor.NewScene();

	EditorViewport& vp = editor.GetViewport();
	vp.HandleResize(glm::uvec2(800, 600), glm::uvec2(800, 600));
	vp.HandleResize(glm::uvec2(0, 0), glm::uvec2(0, 0));

	EXPECT_EQ(vp.Size(), glm::uvec2(800, 600));
	EXPECT_FLOAT_EQ(vp.ViewportCamera()->cam_w, 800.0f);
	EXPECT_FLOAT_EQ(vp.ViewportCamera()->cam_h, 600.0f);
}
