/**
 * @file test_editor_scene_lifecycle.cpp
 * @brief The scene-lifecycle invariant every render pass depends on: the camera
 *        the Editor publishes to the renderer is never null.
 *
 * GeometryPass, LightingPass, SSAOPass, ShadowDepthPass and ShadowIntensityPass
 * all build a view-projection from `EditorContext::camera`, so a null camera
 * either faults or makes the pass skip silently. That makes "the Editor always
 * has a view camera" a precondition of the render graph — and one only the
 * Editor can hold, since it owns every path that creates or empties a scene.
 *
 * The invariant is deliberately NOT "the scene owns a camera". A scene with zero
 * cameras is a legal document: Editor::ViewCamera() returns the *editor camera*
 * (pooled, but not scene content) unless the scene names an activated one, so
 * deleting every scene camera, or loading a project that has none, is fine.
 * What must survive every path is the editor camera itself, which
 * Editor::EnsureEditorCamera() re-establishes after each
 * ResourceManager::Clear().
 *
 * File -> New is the path pinned here: it used to construct a bare `Scene`,
 * which sent a camera-less scene to DrawFrame on the very next timer tick and
 * segfaulted inside ShadowDepthPass.
 *
 * No GPU and no Vulkan: `Editor(nullptr, nullptr)` skips every upload path
 * (each one is guarded on ed_renderer / ed_uploadManager), so this runs in CI.
 * The starter assets are CPU-side loads resolved against the CWD, which CTest
 * pins to the build dir (res/ is copied there by a POST_BUILD step).
 */

#include <gtest/gtest.h>

#include "editor/Editor.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

using namespace neurus;

namespace
{
/// Same starter mesh Application passes to NewScene() / CreateDefaultScene().
constexpr const char* kObj = "res/obj/sphere.obj";
} // namespace

// ---------------------------------------------------------------------------
// 1. NewScene() leaves the scene renderable
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewScene_LeavesARenderableViewCamera)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	const Camera* cam = editor.ViewCamera();
	ASSERT_NE(cam, nullptr)
		<< "A null view camera faults or silently blanks every pass that builds "
		   "a view-projection (ShadowDepthPass in the default graph).";
	EXPECT_EQ(editor.GetScene().cam_list.size(), 1u)
		<< "New should seed exactly one scene camera, not accumulate them.";

	// The seeded scene camera is demo content, left DEACTIVATED: the view comes
	// from the editor camera until the user activates a scene camera.
	EXPECT_EQ(editor.GetScene().GetActiveCamera(), nullptr);
	EXPECT_EQ(editor.GetScene().ActiveCameraID(), 0);
	EXPECT_NE(cam->GetObjectID(), editor.GetScene().cam_list.begin()->first);
	EXPECT_EQ(cam->GetObjectID(), editor.EditorCameraID());
}

// ---------------------------------------------------------------------------
// 2. The invariant survives repeated New — the pool is cleared each time
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, RepeatedNewScene_KeepsExactlyOneCamera)
{
	Editor editor(nullptr, nullptr);

	for (int i = 0; i < 3; ++i)
	{
		editor.NewScene(kObj);
		// ResourceManager::Clear() drops the editor camera too; EnsureEditorCamera()
		// has to re-establish it on every New or the viewport goes black.
		ASSERT_NE(editor.ViewCamera(), nullptr) << "iteration " << i;
		// A stale camera from the previous scene would show up here as a second entry.
		EXPECT_EQ(editor.GetScene().cam_list.size(), 1u) << "iteration " << i;
	}
}

// ---------------------------------------------------------------------------
// 3. The seeded camera is usable, not just present
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewScene_SeededCameraIsRegisteredAndUsable)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	Scene& scene = editor.GetScene();
	ASSERT_EQ(scene.cam_list.size(), 1u);
	const Camera* cam = scene.cam_list.begin()->second.get();
	ASSERT_NE(cam, nullptr);

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
	const glm::mat4 viewView = editor.ViewCamera()->GetViewMatrix();
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			EXPECT_TRUE(std::isfinite(viewView[c][r]));
}

// ---------------------------------------------------------------------------
// 4. New is a fresh DOCUMENT, not an empty one
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewScene_SeedsVisibleStarterContent)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	Scene& scene = editor.GetScene();
	// A camera alone renders solid black with an empty outliner, which reads as
	// a crash. New builds the same starter content as a first launch.
	EXPECT_FALSE(scene.mesh_list.empty()) << "no mesh: nothing to shade";
	EXPECT_FALSE(scene.light_list.empty()) << "no light: the mesh renders unlit";
	EXPECT_FALSE(scene.env_list.empty()) << "no environment: black background, IBL disabled";

	// Not dirty: the starter content is the new document's baseline, so New must
	// not immediately report unsaved changes.
	EXPECT_FALSE(editor.IsDirty());
}

// ---------------------------------------------------------------------------
// 5. Every seeded object carries a display name
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewScene_SeededObjectsAreNamed)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

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
// 6. The camera New seeds is framed for the viewport, not for 1x1
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewScene_SeededCameraAdoptsTheViewportExtent)
{
	// HandleResize is the only place the viewport extent enters the Editor, and it
	// arrives long before File > New. A freshly created Camera defaults to
	// cam_w = cam_h = 1, i.e. a square 1:1 projection, so a New on a 16:9 window
	// used to render the scene horizontally squashed until the next resize
	// happened to poke the camera. HandleResize therefore caches the extent
	// unconditionally and NewScene replays it onto the cameras it just built.
	constexpr uint32_t kW = 1280;
	constexpr uint32_t kH = 720;

	Editor editor(nullptr, nullptr);
	editor.HandleResize({kW, kH}, {kW, kH});  // window shown / resized
	editor.NewScene(kObj);                    // ... then File > New

	const Camera* cam = editor.ViewCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_FLOAT_EQ(cam->cam_w, static_cast<float>(kW));
	EXPECT_FLOAT_EQ(cam->cam_h, static_cast<float>(kH));

	// The DEACTIVATED scene camera is deliberately left at its own defaults: only
	// the camera in use is re-framed here. A camera picks the extent up when it
	// becomes the view camera, because activation enqueues a CameraResizeEvent
	// for it (SceneController::OnActiveCameraChanged).
	ASSERT_EQ(editor.GetScene().cam_list.size(), 1u);
	EXPECT_EQ(editor.GetScene().GetActiveCamera(), nullptr);
}

// ---------------------------------------------------------------------------
// 7. A New before any resize leaves the camera untouched rather than zeroed
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewScene_WithoutAViewportKeepsANonDegenerateCamera)
{
	// At startup the extent is still 0x0. Replaying that onto the camera would
	// divide by zero in the projection; ApplyViewportToViewCamera bails out
	// instead and leaves the camera's own defaults in place.
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	const Camera* cam = editor.ViewCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_GT(cam->cam_w, 0.0f);
	EXPECT_GT(cam->cam_h, 0.0f);
}
