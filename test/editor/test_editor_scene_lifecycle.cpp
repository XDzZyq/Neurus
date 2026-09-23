/**
 * @file test_editor_scene_lifecycle.cpp
 * @brief The scene-lifecycle invariant every render pass depends on: a scene
 *        the Editor hands to the renderer always owns at least one camera.
 *
 * GeometryPass, LightingPass, SSAOPass, ShadowDepthPass and ShadowIntensityPass
 * all open with `scene->GetActiveCamera()` and dereference the result without a
 * null check, because their whole reason to exist is a view-projection matrix.
 * That makes "the scene has an active camera" a precondition of the render
 * graph, not a nicety — and one that only the Editor can hold, since it owns
 * every path that creates or empties a scene.
 *
 * Two of those paths are guarded elsewhere and stay guarded:
 *   - deletion, by SceneController's last-camera refusal;
 *   - the startup scene, by CreateDefaultScene().
 * File -> New is the third, and it used to construct a bare `Scene`, which sent
 * a camera-less scene to DrawFrame on the very next timer tick and segfaulted
 * inside ShadowDepthPass. These tests pin the invariant at that third path.
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

TEST(EditorSceneLifecycleTest, NewScene_LeavesAnActiveCamera)
{
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	const Camera* cam = editor.GetScene().GetActiveCamera();
	ASSERT_NE(cam, nullptr)
		<< "A camera-less scene faults inside the first pass that builds a "
		   "view-projection (ShadowDepthPass in the default graph).";
	EXPECT_EQ(editor.GetScene().cam_list.size(), 1u)
		<< "New should seed exactly one camera, not accumulate them.";
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
		ASSERT_NE(editor.GetScene().GetActiveCamera(), nullptr) << "iteration " << i;
		// ResourceManager::Clear() runs before the seed, so a stale camera from
		// the previous scene would show up here as a second entry.
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
	const Camera* cam = scene.GetActiveCamera();
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
	// arrives long before File > New. A freshly seeded Camera defaults to
	// cam_w = cam_h = 1, i.e. a square 1:1 projection, so a New on a 16:9 window
	// used to render the scene horizontally squashed until the next resize
	// happened to poke the camera. HandleResize therefore caches the extent
	// unconditionally and NewScene replays it onto the camera it just seeded.
	constexpr uint32_t kW = 1280;
	constexpr uint32_t kH = 720;

	Editor editor(nullptr, nullptr);
	editor.HandleResize({kW, kH}, {kW, kH});  // window shown / resized
	editor.NewScene(kObj);                    // ... then File > New

	const Camera* cam = editor.GetScene().GetActiveCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_FLOAT_EQ(cam->cam_w, static_cast<float>(kW));
	EXPECT_FLOAT_EQ(cam->cam_h, static_cast<float>(kH));
}

// ---------------------------------------------------------------------------
// 7. A New before any resize leaves the camera untouched rather than zeroed
// ---------------------------------------------------------------------------

TEST(EditorSceneLifecycleTest, NewScene_WithoutAViewportKeepsANonDegenerateCamera)
{
	// At startup the extent is still 0x0. Replaying that onto the camera would
	// divide by zero in the projection; ApplyViewportToActiveCamera bails out
	// instead and leaves the camera's own defaults in place.
	Editor editor(nullptr, nullptr);
	editor.NewScene(kObj);

	const Camera* cam = editor.GetScene().GetActiveCamera();
	ASSERT_NE(cam, nullptr);
	EXPECT_GT(cam->cam_w, 0.0f);
	EXPECT_GT(cam->cam_h, 0.0f);
}
