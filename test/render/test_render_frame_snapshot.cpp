/**
 * @file test_render_frame_snapshot.cpp
 * @brief The ownership contract of RenderFrameSnapshot: a frame, detached.
 *
 * `RenderContext` publishes borrowed `Scene` / `Camera` pointers, which is only
 * safe while the Editor and the renderer share one thread. A render thread reads
 * those pointers while the Editor is already editing them, so every per-frame
 * value the render layer needs has to be *copied* out of the Scene while the
 * Editor still owns it, and resource references have to survive as stable keys
 * rather than as pointers into pools the Editor may re-key.
 *
 * Two things are pinned here, and nothing else can pin them:
 *
 *   1. The snapshot *is* the frame: counts, transforms and stable keys match the
 *      Scene it was built from. A snapshot that quietly drops an object renders
 *      a plausible-looking wrong frame rather than failing.
 *   2. The snapshot is *detached*: mutating the Scene, the camera or the overlay
 *      lists afterwards — moving a mesh, moving a light, deleting a light,
 *      moving the eye, re-flattening the overlays, changing the config — leaves
 *      the already-built snapshot carrying the *pre-mutation* values. This is
 *      the whole point of the type; a borrowed pointer passes test 1 and fails
 *      this one.
 *
 * Every detachment assertion below is written as a pair, deliberately: the
 * snapshot's field against the value captured from the *live* object before the
 * mutation (or against a literal), and then `EXPECT_NE` against that same live
 * object's value *after* it. Comparing the snapshot against a copy of itself
 * would hold for any implementation that compiles, which is to say it would
 * assert nothing at all.
 *
 * No GPU and no Vulkan: `Editor(nullptr, nullptr)` skips every upload path (each
 * is guarded on ed_renderer / ed_uploadManager), and the Scene is populated by
 * hand, so the only thing under test is the copy.
 */

#include <gtest/gtest.h>

#include "editor/Editor.h"
#include "editor/viewport/EditorViewport.h"
#include "render/RenderFrameSnapshot.h"
#include "scene/Camera.h"
#include "scene/DebugLine.h"
#include "scene/DebugPoints.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

using namespace neurus;

namespace
{

/**
 * @brief Registers a mesh with a distinguishable transform.
 * @return The mesh's UID — the stable key the renderer resolves MeshGPU through.
 */
int AddMesh(Scene& scene, const glm::vec3& pos, float scale)
{
	auto mesh = std::make_shared<Mesh>();
	mesh->SetPosition(pos);
	mesh->SetScale(glm::vec3(scale));
	scene.UseMesh(mesh);
	return mesh->GetObjectID();
}

/**
 * @brief Registers a light with a distinguishable position and radius.
 * @return The light's UID — the stable key for LightGPU and its shadow layer.
 */
int AddLight(Scene& scene, LightType type, const glm::vec3& pos, float radius)
{
	auto light = std::make_shared<Light>(type);
	light->SetPosition(pos);
	light->SetRadius(radius);
	scene.UseLight(light);
	return light->GetObjectID();
}

/// @brief The snapshot's record for @p uid, or nullptr when it carries none.
const MeshInstanceSnapshot* FindMesh(const RenderFrameSnapshot& snap, int uid)
{
	const auto it = std::find_if(snap.meshes.begin(), snap.meshes.end(),
	                             [uid](const MeshInstanceSnapshot& m) { return m.objectId == uid; });
	return it != snap.meshes.end() ? &*it : nullptr;
}

/// @brief The snapshot's record for @p uid, or nullptr when it carries none.
const LightSnapshot* FindLight(const RenderFrameSnapshot& snap, int uid)
{
	const auto it = std::find_if(snap.lights.begin(), snap.lights.end(),
	                             [uid](const LightSnapshot& l) { return l.objectId == uid; });
	return it != snap.lights.end() ? &*it : nullptr;
}

/**
 * @brief True when @p list holds a segment ending at @p end.
 *
 * Content predicates rather than indices: primitives from different debug
 * objects arrive in unordered_map order, so an absolute index would assert the
 * pool's iteration order instead of the copy.
 */
bool HasSegmentEnd(const DebugDrawList& list, const glm::vec3& end)
{
	return std::any_of(list.segments.begin(), list.segments.end(),
	                   [&end](const DebugSegment& s) { return s.b == end; });
}

/// @brief True when @p list holds a point sprite at @p pos.
bool HasPointAt(const DebugDrawList& list, const glm::vec3& pos)
{
	return std::any_of(list.points.begin(), list.points.end(),
	                   [&pos](const DebugPointSprite& p) { return p.p == pos; });
}

/** @brief The two meshes and three lights every case below is built on. */
struct Populated
{
	explicit Populated(Editor& editor)
	{
		editor.NewScene();
		Scene& scene = editor.GetScene();

		meshA = AddMesh(scene, glm::vec3(1.0f, 2.0f, 3.0f), 1.0f);
		meshB = AddMesh(scene, glm::vec3(-4.0f, 0.5f, 7.0f), 2.0f);

		// One of each shadow-casting type the passes branch on, so a snapshot
		// that collapses the type field cannot pass.
		lightPoint = AddLight(scene, LightType::POINTLIGHT, glm::vec3(0.0f, 0.0f, 5.0f), 0.25f);
		lightSun   = AddLight(scene, LightType::SUNLIGHT, glm::vec3(10.0f, 0.0f, 10.0f), 0.5f);
		lightSpot  = AddLight(scene, LightType::SPOTLIGHT, glm::vec3(-2.0f, -3.0f, 4.0f), 0.75f);
	}

	int meshA = 0;
	int meshB = 0;
	int lightPoint = 0;
	int lightSun = 0;
	int lightSpot = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. The snapshot is the frame: counts, transforms and stable keys
// ---------------------------------------------------------------------------

/**
 * @test A scene of 2 meshes + 3 lights yields a snapshot whose counts, model
 *       matrices and stable keys are the scene's.
 */
TEST(RenderFrameSnapshotTest, BuildSnapshot_MatchesTheScene)
{
	Editor editor(nullptr, nullptr);
	const Populated ids(editor);
	const Scene& scene = editor.GetScene();

	const RenderFrameSnapshot snap = editor.BuildSnapshot();

	ASSERT_TRUE(snap.HasScene());
	ASSERT_EQ(snap.meshes.size(), scene.mesh_list.size()) << "2 meshes in, 2 meshes out";
	ASSERT_EQ(snap.lights.size(), scene.light_list.size()) << "3 lights in, 3 lights out";

	// Stable keys, not pointers: every record names the UID the renderer
	// resolves MeshGPU / LightGPU through.
	const MeshInstanceSnapshot* a = FindMesh(snap, ids.meshA);
	const MeshInstanceSnapshot* b = FindMesh(snap, ids.meshB);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);

	EXPECT_EQ(a->model, scene.mesh_list.at(ids.meshA)->GetModelMatrix());
	EXPECT_EQ(b->model, scene.mesh_list.at(ids.meshB)->GetModelMatrix());
	EXPECT_EQ(a->normalMatrix, scene.mesh_list.at(ids.meshA)->GetNormalMatrix());

	const LightSnapshot* point = FindLight(snap, ids.lightPoint);
	const LightSnapshot* sun   = FindLight(snap, ids.lightSun);
	const LightSnapshot* spot  = FindLight(snap, ids.lightSpot);
	ASSERT_NE(point, nullptr);
	ASSERT_NE(sun, nullptr);
	ASSERT_NE(spot, nullptr);

	EXPECT_EQ(point->type, LightType::POINTLIGHT);
	EXPECT_EQ(sun->type, LightType::SUNLIGHT);
	EXPECT_EQ(spot->type, LightType::SPOTLIGHT);
	EXPECT_EQ(point->position, scene.light_list.at(ids.lightPoint)->GetPosition());
	EXPECT_EQ(sun->direction, scene.light_list.at(ids.lightSun)->GetDirection());
	EXPECT_FLOAT_EQ(spot->radius, scene.light_list.at(ids.lightSpot)->light_radius);

	// The camera half is the one the viewport publishes, by value.
	const Camera* cam = editor.GetViewport().GetCamera();
	ASSERT_NE(cam, nullptr);
	ASSERT_TRUE(snap.HasCamera());
	EXPECT_EQ(snap.camera.view, cam->GetViewMatrix());
	EXPECT_EQ(snap.camera.projection, cam->GetProjectionMatrix());
	EXPECT_EQ(snap.camera.position, cam->GetPosition());
	EXPECT_EQ(snap.camera.target, cam->cam_tar);
	EXPECT_FLOAT_EQ(snap.camera.nearPlane, cam->cam_near);
	EXPECT_FLOAT_EQ(snap.camera.farPlane, cam->cam_far);

	// Draw order is the UID order, not the unordered_map's: the layer and
	// descriptor-slot assignments downstream are index-based, so a frame must
	// not reorder against an identical scene.
	EXPECT_TRUE(std::is_sorted(snap.meshes.begin(), snap.meshes.end(),
	                           [](const MeshInstanceSnapshot& l, const MeshInstanceSnapshot& r)
	                           { return l.objectId < r.objectId; }));
	EXPECT_TRUE(std::is_sorted(snap.lights.begin(), snap.lights.end(),
	                           [](const LightSnapshot& l, const LightSnapshot& r)
	                           { return l.objectId < r.objectId; }));
}

// ---------------------------------------------------------------------------
// 2. The snapshot is detached: the Scene moving does not move it
// ---------------------------------------------------------------------------

/**
 * @test An already-built snapshot still reports the *pre-mutation* frame after
 *       the Scene, the camera and the config have all moved on.
 *
 * The one test a borrowed-pointer context cannot pass, and the reason the type
 * exists: this is the use-after-free a render thread would otherwise read. Each
 * field is checked twice — equal to what it was built from, and *not* equal to
 * what the live object says now — because only the second half fails for an
 * implementation that reads through to the Scene.
 */
TEST(RenderFrameSnapshotTest, BuildSnapshot_SurvivesLaterSceneMutation)
{
	Editor editor(nullptr, nullptr);
	const Populated ids(editor);
	Scene& scene = editor.GetScene();

	// The camera the viewport publishes is its own while no scene camera is
	// activated, so mutating it through ViewportCamera() is observable in the
	// next snapshot. Asserted rather than assumed: if SyncCamera ever chose
	// differently, the camera half below would silently stop discriminating.
	Camera* cam = editor.GetViewport().ViewportCamera();
	ASSERT_NE(cam, nullptr);
	ASSERT_EQ(cam, editor.GetViewport().GetCamera());

	// What the frame is expected to carry, captured from the *live* objects
	// before anything moves - never off the snapshot, which would compare it
	// against a copy of itself and hold for any implementation that compiles.
	const glm::mat4 meshAModelBefore = scene.mesh_list.at(ids.meshA)->GetModelMatrix();
	const glm::mat4 meshBModelBefore = scene.mesh_list.at(ids.meshB)->GetModelMatrix();
	const glm::vec3 pointPosBefore   = scene.light_list.at(ids.lightPoint)->GetPosition();
	const glm::mat4 camViewBefore    = cam->GetViewMatrix();
	const glm::vec3 camEyeBefore     = cam->GetPosition();
	const glm::vec3 camTargetBefore  = cam->cam_tar;
	const float biasBefore           = editor.GetRenderConfig().r_shadow_bias;

	// Pin the expectations themselves to the literals Populated set up, so a
	// fixture that silently stopped placing the objects cannot make the
	// comparisons below vacuous.
	ASSERT_EQ(glm::vec3(meshAModelBefore[3]), glm::vec3(1.0f, 2.0f, 3.0f));
	ASSERT_EQ(pointPosBefore, glm::vec3(0.0f, 0.0f, 5.0f));
	ASSERT_FLOAT_EQ(scene.light_list.at(ids.lightSpot)->light_radius, 0.75f);

	const RenderFrameSnapshot snap = editor.BuildSnapshot();

	// Now edit the scene out from under it, the way the Editor would while a
	// render thread is mid-frame.
	scene.mesh_list.at(ids.meshA)->SetPosition(glm::vec3(100.0f, 100.0f, 100.0f));
	scene.mesh_list.at(ids.meshB)->SetScale(glm::vec3(9.0f));
	scene.light_list.at(ids.lightPoint)->SetPosition(glm::vec3(-50.0f, 0.0f, 0.0f));
	scene.light_list.at(ids.lightSpot)->SetRadius(42.0f);
	scene.light_list.at(ids.lightSun)->SetShadow(false);
	cam->SetPosition(glm::vec3(0.0f, -80.0f, 40.0f));    // the eye moves too
	cam->SetTarPos(glm::vec3(7.0f, 1.0f, -2.0f));
	AddMesh(scene, glm::vec3(5.0f, 5.0f, 5.0f), 3.0f);   // a whole new object
	scene.RemoveLight(ids.lightSpot);                     // and one gone
	editor.GetRenderConfig().r_shadow_bias = 0.5f;

	// --- Meshes: the transform it was built from, not the one the Scene holds now
	const MeshInstanceSnapshot* a = FindMesh(snap, ids.meshA);
	const MeshInstanceSnapshot* b = FindMesh(snap, ids.meshB);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);

	EXPECT_EQ(a->model, meshAModelBefore) << "a moved mesh must not move the snapshot";
	EXPECT_EQ(b->model, meshBModelBefore);
	EXPECT_NE(a->model, scene.mesh_list.at(ids.meshA)->GetModelMatrix())
	    << "the snapshot must have diverged from the live Scene";
	EXPECT_NE(b->model, scene.mesh_list.at(ids.meshB)->GetModelMatrix());

	// --- Lights: a moved one keeps its pose, and a *deleted* one is still here
	const LightSnapshot* point = FindLight(snap, ids.lightPoint);
	ASSERT_NE(point, nullptr);
	EXPECT_EQ(point->position, pointPosBefore) << "a moved light must not move the snapshot";
	EXPECT_NE(point->position, scene.light_list.at(ids.lightPoint)->GetPosition());

	const LightSnapshot* spot = FindLight(snap, ids.lightSpot);
	ASSERT_NE(spot, nullptr) << "a light deleted after the snapshot was built must still be in it";
	EXPECT_FLOAT_EQ(spot->radius, 0.75f) << "and with the radius it had when it was copied";
	EXPECT_EQ(scene.light_list.count(ids.lightSpot), 0u) << "while the Scene no longer knows it";

	const LightSnapshot* sun = FindLight(snap, ids.lightSun);
	ASSERT_NE(sun, nullptr);
	EXPECT_TRUE(sun->useShadow) << "the flag the sun had when it was copied";
	EXPECT_NE(sun->useShadow, scene.light_list.at(ids.lightSun)->use_shadow);

	// --- Camera: by value, so the eye moving leaves the frame where it was
	EXPECT_EQ(snap.camera.view, camViewBefore);
	EXPECT_EQ(snap.camera.position, camEyeBefore);
	EXPECT_EQ(snap.camera.target, camTargetBefore);
	EXPECT_NE(snap.camera.view, cam->GetViewMatrix()) << "the camera is copied, not borrowed";
	EXPECT_NE(snap.camera.position, cam->GetPosition());

	// --- Config: a value copy, so an edited setting does not reach a built frame
	EXPECT_FLOAT_EQ(snap.Settings().r_shadow_bias, biasBefore)
	    << "the config is a value copy, not a pointer";
	EXPECT_NE(snap.Settings().r_shadow_bias, editor.GetRenderConfig().r_shadow_bias);

	// Counts too: registering and removing objects cannot resize a built frame.
	EXPECT_EQ(snap.meshes.size(), 2u);
	EXPECT_EQ(snap.lights.size(), 3u);

	// The next snapshot, by contrast, must see all of it.
	const RenderFrameSnapshot next = editor.BuildSnapshot();
	EXPECT_EQ(next.meshes.size(), 3u);
	EXPECT_EQ(next.lights.size(), 2u);
	EXPECT_FLOAT_EQ(next.Settings().r_shadow_bias, 0.5f);
	EXPECT_EQ(FindLight(next, ids.lightSpot), nullptr) << "the deleted light is gone from the next frame";
	ASSERT_NE(FindMesh(next, ids.meshA), nullptr);
	EXPECT_EQ(FindMesh(next, ids.meshA)->model,
	          scene.mesh_list.at(ids.meshA)->GetModelMatrix());
	EXPECT_EQ(next.camera.view, cam->GetViewMatrix());
}

// ---------------------------------------------------------------------------
// 3. The fields that are not a list: selection, environment, overlays
// ---------------------------------------------------------------------------

/**
 * @test The overlay payloads are copied by value, and debug draw follows
 *       r_debug_draw.
 *
 * Mirrors UpdateRenderContext's publication rule: the gizmo list is published
 * unconditionally (interaction feedback is not hideable), the debug list is
 * gated on the config flag.
 *
 * Both lists are populated with real geometry first. An empty list would make
 * the copy untestable: "copied nothing" and "copied nothing because there was
 * nothing" are the same bytes, and the gated-off case would be indistinguishable
 * from the gated-on one.
 */
TEST(RenderFrameSnapshotTest, BuildSnapshot_GatesDebugDrawAndCopiesOverlays)
{
	Editor editor(nullptr, nullptr);
	const Populated ids(editor);
	Scene& scene = editor.GetScene();
	EditorViewport& vp = editor.GetViewport();

	// Two segments and three points of debug geometry, at the identity transform
	// so the world-space positions the builder bakes are the ones pushed here.
	auto line = std::make_shared<DebugLine>();
	line->PushDebugLine(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	line->PushDebugLine(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	scene.UseDebugLine(line);

	auto points = std::make_shared<DebugPoints>();
	points->PushDebugPoint(glm::vec3(2.0f, 0.0f, 0.0f));
	points->PushDebugPoint(glm::vec3(3.0f, 0.0f, 0.0f));
	points->PushDebugPoint(glm::vec3(4.0f, 0.0f, 0.0f));
	scene.UseDebugPoints(points);

	// The gizmo builder early-outs on an invalid viewport and on "nothing armed,
	// nothing selected", so the resting handle needs both an extent and an active
	// object. Its pivot is that object's live position - meshA's (1, 2, 3).
	vp.HandleResize(glm::uvec2(800u, 600u), glm::uvec2(800u, 600u));
	scene.selections.Select(scene.GetObjectID(ids.meshA), false);

	vp.MarkOverlaysDirty();
	vp.RebuildOverlays(scene);

	// The fixture itself, before anything about the snapshot is claimed.
	ASSERT_EQ(vp.DebugList().segments.size(), 2u) << "the fixture must produce real geometry";
	ASSERT_EQ(vp.DebugList().points.size(), 3u);
	ASSERT_FALSE(vp.GizmoList().Empty()) << "an active object must raise the resting handle";
	ASSERT_FALSE(vp.GizmoList().points.empty());

	const uint64_t debugRevisionBefore = vp.DebugList().revision;
	const size_t gizmoSegmentsBefore   = vp.GizmoList().segments.size();
	const size_t gizmoPointsBefore     = vp.GizmoList().points.size();
	// Pushed last of everything by the builder, so the back point is the pivot.
	const glm::vec3 gizmoPivotBefore   = vp.GizmoList().points.back().p;
	ASSERT_EQ(gizmoPivotBefore, glm::vec3(1.0f, 2.0f, 3.0f)) << "the pivot is meshA's position";

	editor.GetRenderConfig().r_debug_draw = true;
	const RenderFrameSnapshot on = editor.BuildSnapshot();

	// --- The geometry actually travelled
	ASSERT_TRUE(on.HasDebugDraw());
	EXPECT_EQ(on.debugDraw.segments.size(), 2u) << "the debug segments are copied, not dropped";
	EXPECT_EQ(on.debugDraw.points.size(), 3u);
	EXPECT_EQ(on.debugDraw.revision, debugRevisionBefore);
	EXPECT_TRUE(HasSegmentEnd(on.debugDraw, glm::vec3(1.0f, 0.0f, 0.0f)));
	EXPECT_TRUE(HasSegmentEnd(on.debugDraw, glm::vec3(0.0f, 1.0f, 0.0f)));
	EXPECT_TRUE(HasPointAt(on.debugDraw, glm::vec3(2.0f, 0.0f, 0.0f)));
	EXPECT_TRUE(HasPointAt(on.debugDraw, glm::vec3(4.0f, 0.0f, 0.0f)));

	EXPECT_EQ(on.gizmoDraw.segments.size(), gizmoSegmentsBefore);
	EXPECT_EQ(on.gizmoDraw.points.size(), gizmoPointsBefore);
	ASSERT_FALSE(on.gizmoDraw.points.empty());
	EXPECT_EQ(on.gizmoDraw.points.back().p, gizmoPivotBefore);

	// --- And it is a copy: re-flattening both lists differently leaves it alone
	auto extra = std::make_shared<DebugLine>();
	extra->PushDebugLine(glm::vec3(9.0f, 0.0f, 0.0f), glm::vec3(9.0f, 1.0f, 0.0f));
	scene.UseDebugLine(extra);
	scene.mesh_list.at(ids.meshA)->SetPosition(glm::vec3(1.0f, 20.0f, 3.0f));   // moves the pivot
	vp.MarkOverlaysDirty();
	vp.RebuildOverlays(scene);

	ASSERT_EQ(vp.DebugList().segments.size(), 3u) << "the live list must have changed";
	ASSERT_NE(vp.DebugList().revision, debugRevisionBefore);
	ASSERT_FALSE(vp.GizmoList().points.empty());
	ASSERT_NE(vp.GizmoList().points.back().p, gizmoPivotBefore) << "the live pivot must have moved";

	EXPECT_EQ(on.debugDraw.segments.size(), 2u) << "a rebuilt overlay must not resize a built frame";
	EXPECT_EQ(on.debugDraw.points.size(), 3u);
	EXPECT_EQ(on.debugDraw.revision, debugRevisionBefore);
	EXPECT_NE(on.debugDraw.revision, vp.DebugList().revision)
	    << "the debug list is copied, not borrowed";
	EXPECT_FALSE(HasSegmentEnd(on.debugDraw, glm::vec3(9.0f, 1.0f, 0.0f)))
	    << "geometry added after the copy cannot appear in it";

	EXPECT_EQ(on.gizmoDraw.points.back().p, gizmoPivotBefore);
	EXPECT_NE(on.gizmoDraw.points.back().p, vp.GizmoList().points.back().p)
	    << "the gizmo list is copied, not borrowed";

	// --- The gate
	editor.GetRenderConfig().r_debug_draw = false;
	const RenderFrameSnapshot off = editor.BuildSnapshot();
	EXPECT_FALSE(off.HasDebugDraw()) << "r_debug_draw gates publication, as in UpdateRenderContext";
	EXPECT_TRUE(off.debugDraw.Empty()) << "and nothing is copied when the gate is shut";
	EXPECT_FALSE(off.gizmoDraw.Empty()) << "while the gizmo list is published regardless";
}

/**
 * @test The active object travels as a stable id, and "none" is a legal frame.
 */
TEST(RenderFrameSnapshotTest, BuildSnapshot_CarriesTheActiveObjectAsAnId)
{
	Editor editor(nullptr, nullptr);
	const Populated ids(editor);
	Scene& scene = editor.GetScene();

	EXPECT_EQ(editor.BuildSnapshot().activeObjectId, 0) << "nothing selected is a legal frame";

	scene.selections.Select(scene.GetObjectID(ids.meshB), false);
	const RenderFrameSnapshot snap = editor.BuildSnapshot();
	EXPECT_EQ(snap.activeObjectId, ids.meshB);

	// And it stays put when the selection moves on.
	scene.selections.Select(scene.GetObjectID(ids.meshA), false);
	EXPECT_EQ(snap.activeObjectId, ids.meshB);
	EXPECT_EQ(editor.BuildSnapshot().activeObjectId, ids.meshA);
}

/**
 * @test Clear() empties every list and drops every "has" flag, for reuse.
 */
TEST(RenderFrameSnapshotTest, Clear_EmptiesTheFrameForReuse)
{
	Editor editor(nullptr, nullptr);
	[[maybe_unused]] const Populated ids(editor);

	RenderFrameSnapshot snap = editor.BuildSnapshot();
	ASSERT_FALSE(snap.meshes.empty());

	snap.Clear();

	EXPECT_FALSE(snap.HasScene());
	EXPECT_FALSE(snap.HasCamera());
	EXPECT_FALSE(snap.HasEnvironment());
	EXPECT_FALSE(snap.HasDebugDraw());
	EXPECT_TRUE(snap.meshes.empty());
	EXPECT_TRUE(snap.lights.empty());
	EXPECT_EQ(snap.activeObjectId, 0);
	EXPECT_TRUE(snap.debugDraw.Empty());
	EXPECT_TRUE(snap.gizmoDraw.Empty());
}
