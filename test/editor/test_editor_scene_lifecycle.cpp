/**
 * @file test_editor_scene_lifecycle.cpp
 * @brief The scene-lifecycle invariant every render pass depends on: the camera
 *        the Editor publishes to the renderer is never null.
 *
 * GeometryPass, LightingPass, SSAOPass, ShadowDepthPass and ShadowIntensityPass
 * all build a view-projection from `RenderContext::camera`, so a null camera
 * either faults or makes the pass skip silently. That makes "the Editor always
 * has a view camera" a precondition of the render graph — and one only the
 * Editor can hold, since it owns every path that creates or empties a scene.
 *
 * The invariant is deliberately NOT "the scene owns a camera". A scene with zero
 * cameras is a legal document: EditorViewport::GetCamera() publishes the
 * *viewport camera* (pooled, but not scene content) unless the scene names an
 * activated one, so deleting every scene camera, or loading a project that has
 * none, is fine. What must survive every path is the viewport camera itself,
 * which EditorViewport::EnsureCamera() re-establishes after each
 * ResourceManager::Clear().
 *
 * The Editor owns none of that: it holds no camera and exposes no camera API, so
 * every assertion below reaches the camera through editor.GetViewport().
 *
 * File -> New is the path pinned here: it used to construct a bare `Scene`,
 * which sent a camera-less scene to DrawFrame on the very next timer tick and
 * segfaulted inside ShadowDepthPass. Note that File > New is NOT `NewScene()`
 * alone — it is a new *project*: `NewScene()` (an empty document) followed by
 * `CreateDefaultScene()` (the starter content), composed by
 * Application::OnProjectNew(). Both halves are pinned below, separately, because
 * the whole point of the split is that an empty scene is renderable on its own.
 *
 * No GPU and no Vulkan: `Editor(nullptr, nullptr)` skips every upload path
 * (each one is guarded on ed_renderer / ed_uploadManager), so this runs in CI.
 * The starter assets are CPU-side loads resolved against the CWD, which CTest
 * pins to the build dir (res/ is copied there by a POST_BUILD step).
 */

#include <gtest/gtest.h>

#include "editor/Editor.h"
#include "editor/viewport/EditorViewport.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

using namespace neurus;

namespace
{
/// Same starter mesh Application passes to CreateDefaultScene().
constexpr const char* kObj = "res/obj/sphere.obj";

/**
 * @brief The two steps "File > New" means, in the Application's order.
 *
 * Mirrors Application::OnProjectNew() minus the upload and the dirty rebase:
 * a new scene, then the default content put into it.
 */
void NewProject(Editor& editor)
{
	editor.NewScene();
	editor.CreateDefaultScene(kObj);
}
} // namespace

// ---------------------------------------------------------------------------
// 1. NewScene() leaves the scene renderable — with nothing in it
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewScene_LeavesARenderableViewCamera)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene();

	const Camera* cam = editor.GetViewport().GetCamera();
	ASSERT_NE(cam, nullptr)
		<< "A null view camera faults or silently blanks every pass that builds "
		   "a view-projection (ShadowDepthPass in the default graph).";

	// The view camera is the VIEWPORT camera, which is pooled but is not scene
	// content — that is what makes an empty scene a renderable document.
	EXPECT_EQ(cam->GetObjectID(), editor.GetViewport().CameraID());
	EXPECT_TRUE(editor.GetScene().cam_list.empty())
		<< "NewScene() seeds no scene camera; CreateDefaultScene() does.";
	EXPECT_EQ(editor.GetScene().GetActiveCamera(), nullptr);
	EXPECT_EQ(editor.GetScene().ActiveCameraID(), 0);
}

// ---------------------------------------------------------------------------
// 2. NewScene() means EMPTY: no mesh, no light, no camera, nothing
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewScene_LeavesTheSceneEmpty)
{
	Editor editor(nullptr, nullptr);
	NewProject(editor);           // a populated document first,
	editor.NewScene();            // ... which this must wipe entirely.

	const Scene& scene = editor.GetScene();
	EXPECT_TRUE(scene.obj_list.empty()) << "every pool feeds obj_list";
	EXPECT_TRUE(scene.cam_list.empty());
	EXPECT_TRUE(scene.mesh_list.empty());
	EXPECT_TRUE(scene.light_list.empty());
	EXPECT_TRUE(scene.env_list.empty());
	EXPECT_TRUE(scene.dLine_list.empty());
	EXPECT_TRUE(scene.dPoints_list.empty());
	EXPECT_TRUE(scene.dMesh_list.empty());

	// An empty document is its own saved baseline, not an unsaved edit.
	EXPECT_FALSE(editor.IsDirty());
}

// ---------------------------------------------------------------------------
// 3. The invariant survives repeated New — the pool is cleared each time
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, RepeatedNewScene_KeepsExactlyOneViewCamera)
{
	Editor editor(nullptr, nullptr);

	for (int i = 0; i < 3; ++i)
	{
		editor.NewScene();
		// ResourceManager::Clear() drops the viewport camera too; EnsureCamera()
		// has to re-establish it on every New or the viewport goes black.
		ASSERT_NE(editor.GetViewport().GetCamera(), nullptr) << "iteration " << i;
		// A stale camera from the previous scene would show up here as an entry.
		EXPECT_TRUE(editor.GetScene().cam_list.empty()) << "iteration " << i;
	}

	// Same across the composed path, where there IS a scene camera: exactly one,
	// never an accumulation of the previous documents'.
	for (int i = 0; i < 3; ++i)
	{
		NewProject(editor);
		ASSERT_NE(editor.GetViewport().GetCamera(), nullptr) << "iteration " << i;
		EXPECT_EQ(editor.GetScene().cam_list.size(), 1u) << "iteration " << i;
	}
}

// ---------------------------------------------------------------------------
// 4. The camera the default content seeds is usable, not just present
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewProject_SeededCameraIsRegisteredAndUsable)
{
	Editor editor(nullptr, nullptr);
	NewProject(editor);

	Scene& scene = editor.GetScene();
	ASSERT_EQ(scene.cam_list.size(), 1u);
	const Camera* cam = scene.cam_list.begin()->second.get();
	ASSERT_NE(cam, nullptr);

	// The seeded scene camera is demo content, left DEACTIVATED: the view comes
	// from the viewport camera until the user activates a scene camera.
	EXPECT_EQ(scene.GetActiveCamera(), nullptr);
	EXPECT_EQ(scene.ActiveCameraID(), 0);
	EXPECT_NE(cam->GetObjectID(), editor.GetViewport().CameraID());
	EXPECT_EQ(editor.GetViewport().GetCamera()->GetObjectID(), editor.GetViewport().CameraID());

	// Registered in obj_list under its own type, so the Outliner and every
	// UID-keyed lookup (selection, delete, serialization) can find it.
	const ObjectID* obj = scene.GetObjectID(cam->GetObjectID());
	ASSERT_NE(obj, nullptr) << "seeded camera missing from obj_list";
	EXPECT_EQ(obj->o_type, ObjectID::GOType::GO_CAM);

	// The passes do not merely read the pointer, they build matrices from it:
	// a degenerate camera (eye == target) yields a non-finite view matrix.
	EXPECT_NE(cam->GetPosition(), cam->cam_tar);
	const glm::mat4 view = cam->GetViewMatrix();
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			EXPECT_TRUE(std::isfinite(view[c][r])) << "view[" << c << "][" << r << "]";

	// Same for the camera actually in use.
	const glm::mat4 viewView = editor.GetViewport().GetCamera()->GetViewMatrix();
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			EXPECT_TRUE(std::isfinite(viewView[c][r]));
}

// ---------------------------------------------------------------------------
// 5. New is a fresh DOCUMENT, not an empty one — because the Application
//    composes the two halves, not because NewScene() seeds anything
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewProject_SeedsVisibleStarterContent)
{
	Editor editor(nullptr, nullptr);
	NewProject(editor);

	Scene& scene = editor.GetScene();
	// A camera alone renders solid black with an empty outliner, which reads as
	// a crash. File > New builds the same starter content as a first launch.
	EXPECT_FALSE(scene.mesh_list.empty()) << "no mesh: nothing to shade";
	EXPECT_FALSE(scene.light_list.empty()) << "no light: the mesh renders unlit";
	EXPECT_FALSE(scene.env_list.empty()) << "no environment: black background, IBL disabled";
	EXPECT_FALSE(scene.dLine_list.empty()) << "no debug line: the overlay has nothing to draw";
	EXPECT_FALSE(scene.dPoints_list.empty());
	EXPECT_FALSE(scene.dMesh_list.empty());

	// Dirty, because this mutated a scene like any other edit. Whoever means it
	// as a document baseline rebases explicitly (Application::OnProjectNew ->
	// ClearDirty()), so the decision lives with the caller rather than here.
	EXPECT_TRUE(editor.IsDirty());
}

// ---------------------------------------------------------------------------
// 6. CreateDefaultScene() is purely additive — it resets nothing
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, CreateDefaultScene_AddsToTheCurrentSceneWithoutResetting)
{
	Editor editor(nullptr, nullptr);
	NewProject(editor);

	const Scene& scene = editor.GetScene();
	const size_t meshes = scene.mesh_list.size();
	const size_t lights = scene.light_list.size();
	const size_t objects = scene.obj_list.size();
	const int    viewportCam = editor.GetViewport().CameraID();
	ASSERT_GT(meshes, 0u);

	// A second call layers a second copy on top. This is the contract that makes
	// the Application's two-step New correct: the reset is NewScene()'s job and
	// nobody else's, so this must not quietly wipe what is already there.
	editor.CreateDefaultScene(kObj);

	EXPECT_EQ(scene.mesh_list.size(), meshes * 2);
	EXPECT_EQ(scene.light_list.size(), lights * 2);
	EXPECT_EQ(scene.obj_list.size(), objects * 2);
	// And the viewport camera is untouched: only NewScene() re-mints it.
	EXPECT_EQ(editor.GetViewport().CameraID(), viewportCam);
}

// ---------------------------------------------------------------------------
// 7. Every seeded object carries a display name
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewProject_SeededObjectsAreNamed)
{
	Editor editor(nullptr, nullptr);
	NewProject(editor);

	// A blank o_name renders as an empty Outliner row, indistinguishable from a
	// broken entry. Camera and Light used to leave it default-constructed.
	for (const auto& [uid, obj] : editor.GetScene().obj_list)
	{
		ASSERT_NE(obj, nullptr) << "uid " << uid;
		EXPECT_FALSE(obj->o_name.empty())
			<< "unnamed object of type " << static_cast<int>(obj->o_type);
	}
}

// ---------------------------------------------------------------------------
// 8. The camera New seeds is framed for the viewport, not for 1x1
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewProject_SeededCameraAdoptsTheViewportExtent)
{
	// EditorViewport::HandleResize is the only place the viewport extent enters the
	// application at all, and it arrives long before File > New. A freshly created
	// Camera defaults to cam_w = cam_h = 1, i.e. a square 1:1 projection, so a New
	// on a 16:9 window used to render the scene horizontally squashed until the
	// next resize happened to poke the camera. HandleResize therefore caches the
	// extent unconditionally, and the viewport replays it onto the camera in use —
	// immediately for its own camera, and on the next SyncCamera for the scene's.
	constexpr uint32_t kW = 1280;
	constexpr uint32_t kH = 720;

	Editor editor(nullptr, nullptr);
	editor.GetViewport().HandleResize({kW, kH}, {kW, kH});  // window shown / resized
	NewProject(editor);                       // ... then File > New

	const Camera* cam = editor.GetViewport().GetCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_FLOAT_EQ(cam->cam_w, static_cast<float>(kW));
	EXPECT_FLOAT_EQ(cam->cam_h, static_cast<float>(kH));

	// The DEACTIVATED scene camera is deliberately left at its own defaults: only
	// the camera in use is re-framed here. A camera picks the extent up when it
	// becomes the view camera, because activation enqueues a CameraResizeEvent
	// for it (SceneController::OnActiveCameraChanged).
	ASSERT_EQ(editor.GetScene().cam_list.size(), 1u);
	EXPECT_EQ(editor.GetScene().GetActiveCamera(), nullptr);

	// The same holds for a bare new scene, where the viewport camera is the only one.
	editor.NewScene();
	ASSERT_NE(editor.GetViewport().GetCamera(), nullptr);
	EXPECT_FLOAT_EQ(editor.GetViewport().GetCamera()->cam_w, static_cast<float>(kW));
	EXPECT_FLOAT_EQ(editor.GetViewport().GetCamera()->cam_h, static_cast<float>(kH));
}

// ---------------------------------------------------------------------------
// 9. A New before any resize leaves the camera untouched rather than zeroed
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewScene_WithoutAViewportKeepsANonDegenerateCamera)
{
	// At startup the extent is still 0x0. Replaying that onto the camera would
	// divide by zero in the projection; EditorViewport::FrameCamera bails out
	// instead and leaves the camera's own defaults in place.
	Editor editor(nullptr, nullptr);
	editor.NewScene();

	const Camera* cam = editor.GetViewport().GetCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_GT(cam->cam_w, 0.0f);
	EXPECT_GT(cam->cam_h, 0.0f);
}
