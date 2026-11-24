/**
 * @file test_debug_draw_builder.cpp
 * @brief CPU verification of DebugDrawBuilder: flattening, partitioning, revisions.
 *
 * DebugPass is verified on the GPU in test/render/test_debug_pass.cpp, and it
 * trusts three CPU-side contracts that no GPU test can check:
 *
 *   1. Partition — every depth-tested primitive precedes every x-ray one, and
 *      xraySegmentStart / xrayPointStart mark the boundary exactly. DebugPass
 *      turns those two integers into draw ranges, so an off-by-one draws x-ray
 *      geometry with the depth test still on (or the reverse) while both draws
 *      and every pixel count still look plausible.
 *   2. Revision — exactly one Touch() per rebuild. DebugPass skips its upload
 *      when the revision matches the one it last copied, so a missing Touch()
 *      freezes the overlay and a double Touch() re-uploads every frame.
 *   3. World-space baking — positions leave here already multiplied by their
 *      object's model matrix, because DebugDrawList carries no matrix for
 *      segments or sprites.
 *
 * No GPU is required: this is pure data assembly.
 *
 * @note Scene pools are unordered_map, so the order of primitives coming from
 *       *different* objects is unspecified. Every assertion below is therefore
 *       written over counts and predicates, never over an absolute index that
 *       would depend on pool iteration order.
 */

#include <gtest/gtest.h>

#include "asset/data/MeshData.hpp"
#include "editor/viewport/DebugDrawBuilder.hpp"
#include "scene/DebugLine.hpp"
#include "scene/DebugMesh.hpp"
#include "scene/DebugPoints.hpp"
#include "scene/Scene.hpp"

#include <glm/glm.hpp>

#include "asset/ResourceManager.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

using namespace neurus;

namespace {

/// @brief A DebugLine holding `pairs` unit segments laid end to end along +X.
std::shared_ptr<DebugLine> MakeLine(ResourceManager& resources, size_t pairs, bool xray)
{
	auto line = resources.Load<DebugLine>();
	for (size_t i = 0; i < pairs; ++i)
	{
		const float f = static_cast<float>(i);
		line->PushDebugLine(glm::vec3(f, 0.0f, 0.0f), glm::vec3(f + 1.0f, 0.0f, 0.0f));
	}
	line->SetXRay(xray);
	return line;
}

/// @brief A DebugPoints holding `n` points, one per unit along +X.
std::shared_ptr<DebugPoints> MakePoints(ResourceManager& resources, size_t n, bool xray)
{
	auto pts = resources.Load<DebugPoints>();
	for (size_t i = 0; i < n; ++i)
		pts->PushDebugPoint(glm::vec3(static_cast<float>(i), 0.0f, 0.0f));
	pts->SetXRay(xray);
	return pts;
}

/// @brief True when `bit` is set in `flags`.
bool Has(uint32_t flags, uint32_t bit) { return (flags & bit) != 0u; }

} // namespace

// ===========================================================================
// A. Dirty tracking and revision semantics
// ===========================================================================



/**
 * @test A clean builder ignores Rebuild entirely — including scene changes made
 *       without MarkDirty. This is what makes the per-frame call free.
 */
TEST(DebugDrawBuilder, CleanRebuild_IsNoOp)
{
	ResourceManager resources;
	Scene scene;
	scene.UseDebugLine(MakeLine(resources, 1, false));

	DebugDrawBuilder b;
	EXPECT_TRUE(b.IsDirty());
	EXPECT_TRUE(b.List().Empty());
	const auto initialRevision = b.List().revision;
	b.Rebuild(scene);
	EXPECT_FALSE(b.IsDirty());
	EXPECT_EQ(b.List().revision, initialRevision + 1);
	const uint64_t rev = b.List().revision;
	ASSERT_EQ(b.List().segments.size(), 1u);

	scene.UseDebugLine(MakeLine(resources, 3, false));   // 3 more segments, but no MarkDirty
	b.Rebuild(scene);

	EXPECT_EQ(b.List().segments.size(), 1u) << "A clean builder must not re-flatten";
	EXPECT_EQ(b.List().revision, rev) << "A no-op rebuild must not bump the revision";

	b.MarkDirty();
	b.Rebuild(scene);

	EXPECT_EQ(b.List().segments.size(), 4u) << "MarkDirty must pick up the new object";
	EXPECT_EQ(b.List().revision, rev + 1);
}

/**
 * @test The revision never repeats a value, even when a rebuild reproduces
 *       identical geometry.
 *
 * DebugPass compares revisions for equality, so a repeated value would make it
 * skip an upload that was actually needed. Rebuilding the same scene four times
 * must also leave the list the same size: Rebuild clears before refilling.
 */
TEST(DebugDrawBuilder, Revision_IsMonotonicAcrossIdenticalRebuilds)
{
	ResourceManager resources;
	Scene scene;
	scene.UseDebugLine(MakeLine(resources, 2, false));

	DebugDrawBuilder b;
	uint64_t prev = b.List().revision;
	for (int i = 0; i < 4; ++i)
	{
		b.MarkDirty();
		b.Rebuild(scene);
		EXPECT_GT(b.List().revision, prev) << "Revision repeated on rebuild " << i;
		prev = b.List().revision;
		EXPECT_EQ(b.List().segments.size(), 2u) << "Rebuild must clear before refilling";
	}
}

/**
 * @test DebugDrawList::Clear deliberately does not Touch().
 *
 * The builder clears and refills within one Rebuild, and that must count as one
 * revision rather than two — otherwise every rebuild would advance the revision
 * twice and the "did anything change" comparison would still work but the
 * documented one-Touch-per-rebuild contract would be silently false.
 */
TEST(DebugDrawList, Clear_DoesNotTouch)
{
	DebugDrawList list;
	list.segments.push_back(DebugSegment{});
	list.xraySegmentStart = 1u;
	list.Touch();
	const uint64_t rev = list.revision;

	list.Clear();

	EXPECT_TRUE(list.Empty());
	EXPECT_EQ(list.xraySegmentStart, 0u);
	EXPECT_EQ(list.revision, rev) << "Clear() must leave the revision alone";
}

/// @test PackDebugColor packs 0xAABBGGRR and clamps out-of-range channels.
TEST(DebugDrawList, PackDebugColor_LayoutAndClamping)
{
	EXPECT_EQ(PackDebugColor(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)), 0xFF0000FFu) << "red";
	EXPECT_EQ(PackDebugColor(glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)), 0xFFFF0000u) << "blue";
	EXPECT_EQ(PackDebugColor(glm::vec4(2.0f, -1.0f, 0.0f, 0.0f)), 0x000000FFu) << "clamped";
}

// ===========================================================================
// B. Segment flattening
// ===========================================================================

/**
 * @test Vertices are consumed in endpoint pairs; a trailing odd vertex is
 *       dropped rather than becoming a degenerate segment.
 */
TEST(DebugDrawBuilder, LinesEmitOnlyCompleteVertexPairs)
{
	ResourceManager resources;
	for (const unsigned count : { 1u, 2u, 5u })
	{
		SCOPED_TRACE(count);
		auto line = resources.Load<DebugLine>();
		std::vector<glm::vec3> vertices;
		for (unsigned i = 0; i < count; ++i)
			vertices.emplace_back(static_cast<float>(i), 0.0f, 0.0f);
		line->PushDebugLines(vertices);
		Scene scene;
		scene.UseDebugLine(line);
		DebugDrawBuilder builder;
		builder.Rebuild(scene);
		EXPECT_EQ(builder.List().segments.size(), count / 2u);
		EXPECT_EQ(builder.List().Empty(), count < 2u);
	}
}


/**
 * @test Width and the stipple switch reach the segment, and opacity is folded
 *       into the packed alpha instead of travelling as its own field.
 */
TEST(DebugDrawBuilder, Line_CarriesWidthFlagsAndTintedColor)
{
	ResourceManager resources;
	auto line = resources.Load<DebugLine>();
	line->PushDebugLine(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	line->SetPosition(glm::vec3(10.0f, -3.0f, 2.0f));
	line->SetWidth(4.0f);
	line->SetColor(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
	line->SetOpacity(0.5f);
	line->SetStipple(true);

	Scene scene;
	scene.UseDebugLine(line);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().segments.size(), 1u);
	const DebugSegment& s = b.List().segments[0];
	EXPECT_FLOAT_EQ(s.width, 4.0f);
	EXPECT_TRUE(Has(s.flags, DebugFlag::Stipple));
	EXPECT_FALSE(Has(s.flags, DebugFlag::XRay));

	// 0xAABBGGRR: opaque red at 50% opacity -> alpha 128, red 255.
	EXPECT_EQ(s.rgba, 0x800000FFu) << "Opacity must multiply into the packed alpha";
	EXPECT_EQ(s.a, glm::vec3(10.0f, -3.0f, 2.0f));
	EXPECT_EQ(s.b, glm::vec3(11.0f, -3.0f, 2.0f));
}

/**
 * @test Smoothing is not an authored property: it follows the line style, on for
 *       a solid line and off for a dashed one.
 *
 * Antialiasing fades alpha towards the primitive boundary, which would soften
 * the hard ends a dash is made of — so the two are mutually exclusive rather
 * than independently switchable, and nothing in the Scene stores a smooth bit.
 */
TEST(DebugDrawBuilder, Line_SmoothingFollowsTheLineStyle)
{
	ResourceManager resources;
	auto solid = resources.Load<DebugLine>();
	solid->PushDebugLine(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

	auto dashed = resources.Load<DebugLine>();
	dashed->PushDebugLine(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	dashed->SetStipple(true);

	Scene scene;
	scene.UseDebugLine(solid);
	scene.UseDebugLine(dashed);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().segments.size(), 2u);

	// The pools are unordered_maps, so the two objects can flatten in either
	// order — tell them apart by their axis, not by index.
	const DebugSegment* a = nullptr;   // the solid one, along +X
	const DebugSegment* d = nullptr;   // the dashed one, along +Y
	for (const DebugSegment& s : b.List().segments)
		(s.b.x > 0.5f ? a : d) = &s;
	ASSERT_NE(a, nullptr);
	ASSERT_NE(d, nullptr);

	// Default-constructed: nothing asked for smoothing, it is on regardless.
	EXPECT_TRUE(Has(a->flags, DebugFlag::Smooth)) << "A solid line is antialiased by default";
	EXPECT_FALSE(Has(a->flags, DebugFlag::Stipple));

	EXPECT_TRUE(Has(d->flags, DebugFlag::Stipple));
	EXPECT_FALSE(Has(d->flags, DebugFlag::Smooth)) << "Stipple must turn smoothing off";
}

// ===========================================================================
// C. The x-ray partition — the contract DebugPass turns into draw ranges
// ===========================================================================

/**
 * @test All depth-tested segments precede all x-ray ones, with xraySegmentStart
 *       exactly at the boundary, whatever order the objects were authored in.
 */
TEST(DebugDrawBuilder, MixedDepthModesPartitionBothSegmentsAndPoints)
{
	ResourceManager resources;
	Scene scene;
	scene.UseDebugLine(MakeLine(resources, 3, /*xray=*/false));
	scene.UseDebugLine(MakeLine(resources, 2, /*xray=*/true));
	scene.UseDebugLine(MakeLine(resources, 4, /*xray=*/false));
	scene.UseDebugLine(MakeLine(resources, 1, /*xray=*/true));

	scene.UseDebugPoints(MakePoints(resources, 3, false));
	scene.UseDebugPoints(MakePoints(resources, 5, true));

	DebugDrawBuilder b;
	b.Rebuild(scene);

	const DebugDrawList& l = b.List();
	ASSERT_EQ(l.segments.size(), 10u);
	EXPECT_EQ(l.xraySegmentStart, 7u) << "3 + 4 depth-tested segments come first";

	for (size_t i = 0; i < l.segments.size(); ++i)
	{
		EXPECT_EQ(Has(l.segments[i].flags, DebugFlag::XRay), i >= l.xraySegmentStart)
			<< "Segment " << i << " is on the wrong side of the partition";
	}
	ASSERT_EQ(l.points.size(), 8u);
	EXPECT_EQ(l.xrayPointStart, 3u);
	for (size_t i = 0; i < l.points.size(); ++i)
	{
		EXPECT_EQ(Has(l.points[i].flags, DebugFlag::XRay), i >= l.xrayPointStart)
			<< "Point " << i << " is on the wrong side of the partition";
	}
}

/**
 * @test With no x-ray geometry the boundary sits at the end, so DebugPass's
 *       x-ray range is empty and it issues one draw instead of two.
 */
TEST(DebugDrawBuilder, HomogeneousDepthModeSetsPartitionBoundary)
{
	ResourceManager resources;
	for (const bool xray : { false, true })
	{
		SCOPED_TRACE(xray);
		Scene scene;
		scene.UseDebugLine(MakeLine(resources, 3, xray));
		scene.UseDebugPoints(MakePoints(resources, 2, xray));
		DebugDrawBuilder builder;
		builder.Rebuild(scene);
		const auto& list = builder.List();
		EXPECT_EQ(list.segments.size(), 3u);
		EXPECT_EQ(list.points.size(), 2u);
		EXPECT_EQ(list.xraySegmentStart, xray ? 0u : list.segments.size());
		EXPECT_EQ(list.xrayPointStart, xray ? 0u : list.points.size());
	}
}



/**
 * @test The x-ray scratch buffers do not leak between rebuilds.
 *
 * They are members so their capacity survives across frames; if Rebuild forgot
 * to clear them, every rebuild would append the previous frame's x-ray geometry
 * again and the list would grow without bound while the scene stood still.
 */
TEST(DebugDrawBuilder, Rebuild_DoesNotLeakXrayScratch)
{
	ResourceManager resources;
	Scene scene;
	scene.UseDebugLine(MakeLine(resources, 2, true));
	scene.UseDebugPoints(MakePoints(resources, 2, true));

	DebugDrawBuilder b;
	b.Rebuild(scene);
	ASSERT_EQ(b.List().segments.size(), 2u);
	ASSERT_EQ(b.List().points.size(), 2u);

	b.MarkDirty();
	b.Rebuild(scene);

	EXPECT_EQ(b.List().segments.size(), 2u) << "X-ray segments were appended twice";
	EXPECT_EQ(b.List().points.size(), 2u) << "X-ray points were appended twice";
	EXPECT_EQ(b.List().xraySegmentStart, 0u);
}

// ===========================================================================
// D. Point sprites
// ===========================================================================

/// @test PointType maps onto the shader's shape ids, and scale reaches the sprite.
TEST(DebugDrawBuilder, Point_ShapeMatchesPointType)
{
	ResourceManager resources;
	const std::pair<DebugPoints::PointType, uint32_t> cases[] = {
		{DebugPoints::PointType::SQUARE,  DebugPointShape::Square},
		{DebugPoints::PointType::RHOMBUS, DebugPointShape::Rhombus},
		{DebugPoints::PointType::CIR,     DebugPointShape::Circle},
	};

	for (const auto& [type, shape] : cases)
	{
		auto pts = MakePoints(resources, 1, false);
		pts->SetPointType(type);
		pts->SetScale(7.0f);
		Scene scene;
		scene.UseDebugPoints(pts);

		SCOPED_TRACE(static_cast<int>(type));
		for (const bool screenSized : {false, true})
		{
			SCOPED_TRACE(screenSized);
			pts->SetProjectionMode(screenSized ? 0 : 1);
			DebugDrawBuilder b;
			b.Rebuild(scene);

			ASSERT_EQ(b.List().points.size(), 1u);
			EXPECT_EQ(Has(b.List().points[0].flags, DebugFlag::ScreenSpaceSize), screenSized);
			EXPECT_EQ(b.List().points[0].shape, shape);
			EXPECT_FLOAT_EQ(b.List().points[0].size, 7.0f);
		}
	}
}

/// @test Sprite positions are baked to world space, exactly like segment endpoints.
TEST(DebugDrawBuilder, Point_BakesModelMatrixIntoWorldSpace)
{
	ResourceManager resources;
	auto pts = resources.Load<DebugPoints>();
	pts->PushDebugPoint(glm::vec3(1.0f, 2.0f, 3.0f));
	pts->SetPosition(glm::vec3(0.0f, 0.0f, 10.0f));

	Scene scene;
	scene.UseDebugPoints(pts);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().points.size(), 1u);
	EXPECT_EQ(b.List().points[0].p, glm::vec3(1.0f, 2.0f, 13.0f));
	EXPECT_TRUE(Has(b.List().points[0].flags, DebugFlag::Smooth));
}

// ===========================================================================
// E. PointType::CUBE -> 12 segments
// ===========================================================================

/**
 * @test A CUBE point becomes a 12-edge wireframe and no sprite at all.
 *
 * A screen-aligned sprite cannot express a cube's orientation, so this is the
 * one point type that is decomposed. The edges are checked geometrically rather
 * than by index: each must be axis-aligned, one full side long, and have both
 * corners on the cube's surface — four edges along each axis.
 */
TEST(DebugDrawBuilder, CubePoint_DecomposesInto12Edges)
{
	ResourceManager resources;
	auto pts = resources.Load<DebugPoints>();
	pts->PushDebugPoint(glm::vec3(0.0f));
	pts->SetPointType(DebugPoints::PointType::CUBE);
	pts->SetScale(2.0f);   // scale is the full side, so the half extent is 1.0

	Scene scene;
	scene.UseDebugPoints(pts);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	const DebugDrawList& l = b.List();
	EXPECT_TRUE(l.points.empty()) << "CUBE has no sprite form";
	ASSERT_EQ(l.segments.size(), 12u) << "A cube has 12 edges";

	int perAxis[3] = {0, 0, 0};
	for (const DebugSegment& s : l.segments)
	{
		EXPECT_FALSE(Has(s.flags, DebugFlag::ScreenSpaceSize));
		EXPECT_TRUE(Has(s.flags, DebugFlag::Smooth));
		EXPECT_FALSE(Has(s.flags, DebugFlag::Stipple));
		const glm::vec3 d = s.b - s.a;
		int axis = -1, moving = 0;
		for (int k = 0; k < 3; ++k)
		{
			if (std::fabs(d[k]) > 1e-5f) { axis = k; ++moving; }
			EXPECT_NEAR(std::fabs(s.a[k]), 1.0f, 1e-5f) << "Corner off the cube surface";
			EXPECT_NEAR(std::fabs(s.b[k]), 1.0f, 1e-5f) << "Corner off the cube surface";
		}
		ASSERT_EQ(moving, 1) << "Cube edges must be axis-aligned";
		EXPECT_NEAR(std::fabs(d[axis]), 2.0f, 1e-5f) << "Edge is not one side long";
		++perAxis[axis];
	}
	EXPECT_EQ(perAxis[0], 4) << "4 edges run along X";
	EXPECT_EQ(perAxis[1], 4) << "4 edges run along Y";
	EXPECT_EQ(perAxis[2], 4) << "4 edges run along Z";
}

/// @test An x-ray cube's edges join the x-ray half rather than the depth-tested one.
TEST(DebugDrawBuilder, CubePoint_XrayEdgesJoinTheXrayPartition)
{
	ResourceManager resources;
	auto pts = MakePoints(resources, 2, /*xray=*/true);          // 2 cubes = 24 edges
	pts->SetPointType(DebugPoints::PointType::CUBE);

	Scene scene;
	scene.UseDebugLine(MakeLine(resources, 1, /*xray=*/false));  // 1 depth-tested segment
	scene.UseDebugPoints(pts);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	const DebugDrawList& l = b.List();
	ASSERT_EQ(l.segments.size(), 25u) << "1 line segment + 2 x 12 cube edges";
	EXPECT_EQ(l.xraySegmentStart, 1u);
	for (size_t i = l.xraySegmentStart; i < l.segments.size(); ++i)
		EXPECT_TRUE(Has(l.segments[i].flags, DebugFlag::XRay)) << "Edge " << i;
}

// ===========================================================================
// F. Wireframe meshes
// ===========================================================================

/**
 * @test A DebugMesh with geometry contributes one entry carrying its id, its
 *       transform as a matrix, and its tinted color.
 *
 * Wire meshes are the one primitive that is not flattened: the geometry stays in
 * the mesh's own GPU buffers, so the model matrix has to travel here instead of
 * being baked into vertices the CPU never touches.
 */
TEST(DebugDrawBuilder, WireMesh_CarriesIdModelAndTint)
{
	ResourceManager resources;
	auto mesh = resources.Load<DebugMesh>();
	mesh->SetMeshData(resources.Load<MeshData>());
	mesh->SetPosition(glm::vec3(4.0f, 0.0f, 0.0f));
	mesh->SetColor(glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
	mesh->SetXRay(true);

	Scene scene;
	scene.UseDebugMesh(resources.Load<DebugMesh>()); // no geometry to upload
	scene.UseDebugMesh(mesh);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().wireMeshes.size(), 1u);
	const DebugWireMesh& w = b.List().wireMeshes[0];
	EXPECT_EQ(w.meshObjectId, mesh->RequireObjectID());
	EXPECT_EQ(glm::vec3(w.model[3]), glm::vec3(4.0f, 0.0f, 0.0f));
	EXPECT_EQ(w.rgba, 0xFF00FF00u) << "0xAABBGGRR: opaque green";
	EXPECT_TRUE(Has(w.flags, DebugFlag::XRay));
	EXPECT_FALSE(b.List().Empty()) << "A wire mesh alone must not read as empty";
	EXPECT_FALSE(Has(w.flags, DebugFlag::Smooth));
}

/**
 * @test Wire meshes are deliberately not partitioned.
 *
 * There are few of them and DebugPass switches its depth mode per mesh anyway,
 * so the x-ray boundaries describe only segments and points. Asserting this
 * keeps a future "partition everything" change from silently breaking the
 * meaning of xraySegmentStart.
 */
TEST(DebugDrawBuilder, WireMesh_MixedDepthModesStayInAuthoringOrder)
{
	ResourceManager resources;
	auto opaque = resources.Load<DebugMesh>();
	opaque->SetMeshData(resources.Load<MeshData>());
	auto xray = resources.Load<DebugMesh>();
	xray->SetMeshData(resources.Load<MeshData>());
	xray->SetXRay(true);

	Scene scene;
	scene.UseDebugMesh(opaque);
	scene.UseDebugMesh(xray);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	const DebugDrawList& l = b.List();
	ASSERT_EQ(l.wireMeshes.size(), 2u);
	EXPECT_EQ(l.xraySegmentStart, 0u) << "Wire meshes must not move the segment boundary";
	EXPECT_EQ(l.xrayPointStart, 0u) << "Wire meshes must not move the point boundary";

	int xrayCount = 0;
	for (const DebugWireMesh& w : l.wireMeshes)
		if (Has(w.flags, DebugFlag::XRay)) ++xrayCount;
	EXPECT_EQ(xrayCount, 1) << "Each mesh keeps its own depth mode";
}

// ===========================================================================
// G. Visibility flags
// ===========================================================================
//
// ObjectID carries `is_viewport` and `is_rendered`, the pair the Outliner's two
// per-row toggles drive through UIEvents -> VisibilityChanged ->
// ObjectController -> ObjectID::SetVisible. Every other consumer of a scene pool
// (GeometryPass, ShadowDepthPass, UploadManager) skips an object when either
// flag is clear, and the overlay has to do the same here: `DebugDrawList` has no
// per-primitive enable bit, so an invisible object must simply not be flattened.
//
// The rebuild that follows a toggle is already wired -- ObjectController emits
// RenderResetEvent, whose handler calls MarkDirty() -- so these tests pin the
// half that was missing: the filter itself.

/// @test A hidden DebugLine contributes no segments.
TEST(DebugDrawBuilder, HiddenDebugObjectsDoNotEmitGeometry)
{
	ResourceManager resources;
	for (const bool viewportVisible : { false, true })
	{
		SCOPED_TRACE(viewportVisible);
		auto line = MakeLine(resources, 3, false);
		line->SetVisible(viewportVisible, !viewportVisible);
		auto points = MakePoints(resources, 4, false);
		points->SetVisible(viewportVisible, !viewportVisible);
		auto mesh = resources.Load<DebugMesh>();
		mesh->SetMeshData(resources.Load<MeshData>());
		mesh->SetVisible(viewportVisible, !viewportVisible);
		Scene scene;
		scene.UseDebugPoints(points);
		scene.UseDebugMesh(mesh);
		scene.UseDebugLine(line);
		DebugDrawBuilder builder;
		builder.Rebuild(scene);
		EXPECT_TRUE(builder.List().Empty());
	}
}


/**
 * @test A hidden object drops out of the list without disturbing the x-ray
 *       partition of the objects that remain.
 *
 * The boundary is a count, not an index into a fixed layout, so filtering has to
 * happen before the partition is computed -- filtering afterwards would leave
 * xraySegmentStart pointing past the depth-tested range it describes and make
 * DebugPass draw the x-ray half with the depth test still enabled.
 */
TEST(DebugDrawBuilder, Visibility_HidingOneObjectKeepsThePartitionExact)
{
	ResourceManager resources;
	auto hiddenOpaque = MakeLine(resources, 2, false);  // 2 depth-tested segments, hidden
	hiddenOpaque->SetVisible(false, true);

	Scene scene;
	scene.UseDebugLine(hiddenOpaque);
	scene.UseDebugLine(MakeLine(resources, 3, false));  // 3 depth-tested
	scene.UseDebugLine(MakeLine(resources, 1, true));   // 1 x-ray

	DebugDrawBuilder b;
	b.Rebuild(scene);

	const DebugDrawList& l = b.List();
	ASSERT_EQ(l.segments.size(), 4u) << "The hidden line's 2 segments are gone";
	EXPECT_EQ(l.xraySegmentStart, 3u) << "Boundary follows the surviving segments";
	for (uint32_t i = 0; i < l.xraySegmentStart; ++i)
		EXPECT_FALSE(Has(l.segments[i].flags, DebugFlag::XRay));
	for (size_t i = l.xraySegmentStart; i < l.segments.size(); ++i)
		EXPECT_TRUE(Has(l.segments[i].flags, DebugFlag::XRay));
	const auto revision = l.revision;
	hiddenOpaque->SetVisible(true, true);
	b.MarkDirty();
	b.Rebuild(scene);
	EXPECT_EQ(l.segments.size(), 6u);
	EXPECT_EQ(l.xraySegmentStart, 5u);
	EXPECT_GT(l.revision, revision);
}
