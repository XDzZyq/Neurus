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

#include "asset/data/MeshData.h"
#include "editor/DebugDrawBuilder.h"
#include "scene/DebugLine.h"
#include "scene/DebugMesh.h"
#include "scene/DebugPoints.h"
#include "scene/Scene.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

using namespace neurus;

namespace {

/// @brief A DebugLine holding `pairs` unit segments laid end to end along +X.
std::shared_ptr<DebugLine> MakeLine(size_t pairs, bool xray)
{
	auto line = std::make_shared<DebugLine>();
	for (size_t i = 0; i < pairs; ++i)
	{
		const float f = static_cast<float>(i);
		line->PushDebugLine(glm::vec3(f, 0.0f, 0.0f), glm::vec3(f + 1.0f, 0.0f, 0.0f));
	}
	line->SetXRay(xray);
	return line;
}

/// @brief A DebugPoints holding `n` points, one per unit along +X.
std::shared_ptr<DebugPoints> MakePoints(size_t n, bool xray)
{
	auto pts = std::make_shared<DebugPoints>();
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

/// @test A fresh builder is dirty, so the first Edit() populates the list.
TEST(DebugDrawBuilder, StartsDirty)
{
	DebugDrawBuilder b;
	EXPECT_TRUE(b.IsDirty());
	EXPECT_TRUE(b.List().Empty());
}

/// @test A rebuild clears the dirty flag and bumps the revision exactly once.
TEST(DebugDrawBuilder, Rebuild_ClearsDirtyAndTouchesOnce)
{
	Scene scene;
	DebugDrawBuilder b;
	const uint64_t before = b.List().revision;

	b.Rebuild(scene);

	EXPECT_FALSE(b.IsDirty());
	EXPECT_EQ(b.List().revision, before + 1) << "Exactly one Touch() per rebuild";
}

/**
 * @test A clean builder ignores Rebuild entirely — including scene changes made
 *       without MarkDirty. This is what makes the per-frame call free.
 */
TEST(DebugDrawBuilder, CleanRebuild_IsNoOp)
{
	Scene scene;
	scene.UseDebugLine(MakeLine(1, false));

	DebugDrawBuilder b;
	b.Rebuild(scene);
	const uint64_t rev = b.List().revision;
	ASSERT_EQ(b.List().segments.size(), 1u);

	scene.UseDebugLine(MakeLine(3, false));   // 3 more segments, but no MarkDirty
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
	Scene scene;
	scene.UseDebugLine(MakeLine(2, false));

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
TEST(DebugDrawBuilder, Line_VerticesPairIntoSegments)
{
	auto line = std::make_shared<DebugLine>();
	line->PushDebugLines({glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
	                      glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(3.0f, 0.0f, 0.0f),
	                      glm::vec3(4.0f, 0.0f, 0.0f)});   // 5 = 2 pairs + 1 orphan

	Scene scene;
	scene.UseDebugLine(line);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	EXPECT_EQ(b.List().segments.size(), 2u) << "The 5th vertex has no partner";
}

/// @test A lone vertex produces nothing at all.
TEST(DebugDrawBuilder, Line_SingleVertexEmitsNothing)
{
	auto line = std::make_shared<DebugLine>();
	line->PushDebugLines({glm::vec3(0.0f)});

	Scene scene;
	scene.UseDebugLine(line);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	EXPECT_TRUE(b.List().segments.empty()) << "A lone vertex is not a segment";
	EXPECT_TRUE(b.List().Empty());
}

/**
 * @test Endpoints leave the builder already in world space.
 *
 * DebugDrawList carries no matrix for segments, so a transform that is not
 * folded in here is lost outright: the overlay would draw at the origin no
 * matter where its object sits.
 */
TEST(DebugDrawBuilder, Line_BakesModelMatrixIntoWorldSpace)
{
	auto line = std::make_shared<DebugLine>();
	line->PushDebugLine(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	line->SetPosition(glm::vec3(10.0f, -3.0f, 2.0f));

	Scene scene;
	scene.UseDebugLine(line);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().segments.size(), 1u);
	EXPECT_EQ(b.List().segments[0].a, glm::vec3(10.0f, -3.0f, 2.0f));
	EXPECT_EQ(b.List().segments[0].b, glm::vec3(11.0f, -3.0f, 2.0f));
}

/**
 * @test Width and the stipple switch reach the segment, and opacity is folded
 *       into the packed alpha instead of travelling as its own field.
 */
TEST(DebugDrawBuilder, Line_CarriesWidthFlagsAndTintedColor)
{
	auto line = std::make_shared<DebugLine>();
	line->PushDebugLine(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
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
	auto solid = std::make_shared<DebugLine>();
	solid->PushDebugLine(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

	auto dashed = std::make_shared<DebugLine>();
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
TEST(DebugDrawBuilder, Partition_SegmentsSplitAtXraySegmentStart)
{
	Scene scene;
	scene.UseDebugLine(MakeLine(3, /*xray=*/false));
	scene.UseDebugLine(MakeLine(2, /*xray=*/true));
	scene.UseDebugLine(MakeLine(4, /*xray=*/false));
	scene.UseDebugLine(MakeLine(1, /*xray=*/true));

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
}

/**
 * @test With no x-ray geometry the boundary sits at the end, so DebugPass's
 *       x-ray range is empty and it issues one draw instead of two.
 */
TEST(DebugDrawBuilder, Partition_NoXrayPutsBoundaryAtEnd)
{
	Scene scene;
	scene.UseDebugLine(MakeLine(3, false));
	scene.UseDebugPoints(MakePoints(2, false));

	DebugDrawBuilder b;
	b.Rebuild(scene);

	EXPECT_EQ(b.List().xraySegmentStart, b.List().segments.size());
	EXPECT_EQ(b.List().xrayPointStart, b.List().points.size());
}

/// @test All-x-ray geometry puts the boundary at 0: the depth-tested range is empty.
TEST(DebugDrawBuilder, Partition_AllXrayPutsBoundaryAtZero)
{
	Scene scene;
	scene.UseDebugLine(MakeLine(3, true));
	scene.UseDebugPoints(MakePoints(2, true));

	DebugDrawBuilder b;
	b.Rebuild(scene);

	EXPECT_EQ(b.List().segments.size(), 3u);
	EXPECT_EQ(b.List().xraySegmentStart, 0u);
	EXPECT_EQ(b.List().points.size(), 2u);
	EXPECT_EQ(b.List().xrayPointStart, 0u);
}

/// @test Points partition independently: x-ray segments must not shift the points.
TEST(DebugDrawBuilder, Partition_PointsSplitAtXrayPointStart)
{
	Scene scene;
	scene.UseDebugPoints(MakePoints(3, /*xray=*/false));
	scene.UseDebugPoints(MakePoints(5, /*xray=*/true));
	scene.UseDebugLine(MakeLine(2, /*xray=*/true));

	DebugDrawBuilder b;
	b.Rebuild(scene);

	const DebugDrawList& l = b.List();
	ASSERT_EQ(l.points.size(), 8u);
	EXPECT_EQ(l.xrayPointStart, 3u);
	for (size_t i = 0; i < l.points.size(); ++i)
	{
		EXPECT_EQ(Has(l.points[i].flags, DebugFlag::XRay), i >= l.xrayPointStart)
			<< "Point " << i << " is on the wrong side of the partition";
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
	Scene scene;
	scene.UseDebugLine(MakeLine(2, true));
	scene.UseDebugPoints(MakePoints(2, true));

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

/**
 * @test ScreenSpaceSize tracks the projection mode: mode 0 means the scale is a
 *       pixel diameter, mode 1 means world units.
 */
TEST(DebugDrawBuilder, Point_ScreenSpaceFlagFollowsProjectionMode)
{
	auto screen = MakePoints(1, false);
	screen->SetProjectionMode(0);
	auto world = MakePoints(1, false);
	world->SetProjectionMode(1);

	Scene scene;
	scene.UseDebugPoints(screen);
	scene.UseDebugPoints(world);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().points.size(), 2u);
	int screenSpace = 0;
	for (const DebugPointSprite& p : b.List().points)
		if (Has(p.flags, DebugFlag::ScreenSpaceSize)) ++screenSpace;
	EXPECT_EQ(screenSpace, 1) << "Exactly one of the two is screen-space sized";
}

/// @test PointType maps onto the shader's shape ids, and scale reaches the sprite.
TEST(DebugDrawBuilder, Point_ShapeMatchesPointType)
{
	const std::pair<DebugPoints::PointType, uint32_t> cases[] = {
		{DebugPoints::PointType::SQUARE,  DebugPointShape::Square},
		{DebugPoints::PointType::RHOMBUS, DebugPointShape::Rhombus},
		{DebugPoints::PointType::CIR,     DebugPointShape::Circle},
	};

	for (const auto& [type, shape] : cases)
	{
		auto pts = MakePoints(1, false);
		pts->SetPointType(type);
		pts->SetScale(7.0f);

		Scene scene;
		scene.UseDebugPoints(pts);

		DebugDrawBuilder b;
		b.Rebuild(scene);

		ASSERT_EQ(b.List().points.size(), 1u);
		EXPECT_EQ(b.List().points[0].shape, shape);
		EXPECT_FLOAT_EQ(b.List().points[0].size, 7.0f);
	}
}

/// @test Sprite positions are baked to world space, exactly like segment endpoints.
TEST(DebugDrawBuilder, Point_BakesModelMatrixIntoWorldSpace)
{
	auto pts = std::make_shared<DebugPoints>();
	pts->PushDebugPoint(glm::vec3(1.0f, 2.0f, 3.0f));
	pts->SetPosition(glm::vec3(0.0f, 0.0f, 10.0f));

	Scene scene;
	scene.UseDebugPoints(pts);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().points.size(), 1u);
	EXPECT_EQ(b.List().points[0].p, glm::vec3(1.0f, 2.0f, 13.0f));
}

/**
 * @test Every sprite shape is smoothed, unconditionally and for all projection
 *       modes — DebugPoints has no line style that could turn it off.
 *
 * The shape mask in debug_point.frag is an analytic distance (Chebyshev,
 * Manhattan or Euclidean), so a rhombus or circle boundary is visibly stepped
 * without the fade. Nothing authors this, so the assertion is on a plain
 * default-constructed object.
 */
TEST(DebugDrawBuilder, Point_SpritesAreAlwaysSmoothed)
{
	const DebugPoints::PointType shapes[] = {
		DebugPoints::PointType::SQUARE,
		DebugPoints::PointType::RHOMBUS,
		DebugPoints::PointType::CIR,
	};

	for (DebugPoints::PointType type : shapes)
	{
		for (int mode : {0, 1})
		{
			auto pts = MakePoints(1, false);
			pts->SetPointType(type);
			pts->SetProjectionMode(mode);

			Scene scene;
			scene.UseDebugPoints(pts);

			DebugDrawBuilder b;
			b.Rebuild(scene);

			ASSERT_EQ(b.List().points.size(), 1u);
			EXPECT_TRUE(Has(b.List().points[0].flags, DebugFlag::Smooth))
				<< "Sprites are antialiased by default, shape " << static_cast<int>(type)
				<< " projection mode " << mode;
		}
	}
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
	auto pts = std::make_shared<DebugPoints>();
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

/**
 * @test A cube's edges are world-space by definition, so ScreenSpaceSize stays
 *       clear even though the default projection mode is screen space.
 */
TEST(DebugDrawBuilder, CubePoint_EdgesAreNeverScreenSpaceSized)
{
	auto pts = std::make_shared<DebugPoints>();
	pts->PushDebugPoint(glm::vec3(0.0f));
	pts->SetPointType(DebugPoints::PointType::CUBE);
	pts->SetProjectionMode(0);   // the sprite default: pixels

	Scene scene;
	scene.UseDebugPoints(pts);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().segments.size(), 12u);
	for (const DebugSegment& s : b.List().segments)
		EXPECT_FALSE(Has(s.flags, DebugFlag::ScreenSpaceSize));
}

/**
 * @test Cube edges are smoothed and never stippled: they are segments of a solid
 *       style, so they get what a solid DebugLine gets.
 */
TEST(DebugDrawBuilder, CubePoint_EdgesAreSmoothedLikeSolidLines)
{
	auto pts = MakePoints(1, false);
	pts->SetPointType(DebugPoints::PointType::CUBE);

	Scene scene;
	scene.UseDebugPoints(pts);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().segments.size(), 12u);
	for (const DebugSegment& s : b.List().segments)
	{
		EXPECT_TRUE(Has(s.flags, DebugFlag::Smooth));
		EXPECT_FALSE(Has(s.flags, DebugFlag::Stipple));
	}
}

/// @test An x-ray cube's edges join the x-ray half rather than the depth-tested one.
TEST(DebugDrawBuilder, CubePoint_XrayEdgesJoinTheXrayPartition)
{
	auto pts = MakePoints(2, /*xray=*/true);          // 2 cubes = 24 edges
	pts->SetPointType(DebugPoints::PointType::CUBE);

	Scene scene;
	scene.UseDebugLine(MakeLine(1, /*xray=*/false));  // 1 depth-tested segment
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
 * @test A DebugMesh with no geometry is skipped.
 *
 * DebugPass draws wireframes from the mesh's MeshGPU; without geometry there is
 * nothing to reference, and an entry the pass always skips is dead weight in
 * the list.
 */
TEST(DebugDrawBuilder, WireMesh_WithoutGeometryIsSkipped)
{
	Scene scene;
	scene.UseDebugMesh(std::make_shared<DebugMesh>());

	DebugDrawBuilder b;
	b.Rebuild(scene);

	EXPECT_TRUE(b.List().wireMeshes.empty());
	EXPECT_TRUE(b.List().Empty());
}

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
	auto mesh = std::make_shared<DebugMesh>();
	mesh->SetMeshData(std::make_shared<MeshData>());
	mesh->SetPosition(glm::vec3(4.0f, 0.0f, 0.0f));
	mesh->SetColor(glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
	mesh->SetXRay(true);

	Scene scene;
	scene.UseDebugMesh(mesh);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().wireMeshes.size(), 1u);
	const DebugWireMesh& w = b.List().wireMeshes[0];
	EXPECT_EQ(w.meshObjectId, mesh->GetObjectID());
	EXPECT_EQ(glm::vec3(w.model[3]), glm::vec3(4.0f, 0.0f, 0.0f));
	EXPECT_EQ(w.rgba, 0xFF00FF00u) << "0xAABBGGRR: opaque green";
	EXPECT_TRUE(Has(w.flags, DebugFlag::XRay));
	EXPECT_FALSE(b.List().Empty()) << "A wire mesh alone must not read as empty";
}

/**
 * @test A wire mesh carries no Smooth bit, unlike every segment and sprite.
 *
 * Wireframes are rasterized by PolygonMode::eLine, so the fragment stage has no
 * distance-to-edge to fade and debug_wire.frag has no branch on the flag. Pinning
 * the asymmetry keeps "smooth by default" from being read as "set the bit
 * everywhere", which would advertise a behaviour the shader does not implement.
 */
TEST(DebugDrawBuilder, WireMesh_IsNotFlaggedSmooth)
{
	auto mesh = std::make_shared<DebugMesh>();
	mesh->SetMeshData(std::make_shared<MeshData>());

	Scene scene;
	scene.UseDebugMesh(mesh);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	ASSERT_EQ(b.List().wireMeshes.size(), 1u);
	EXPECT_FALSE(Has(b.List().wireMeshes[0].flags, DebugFlag::Smooth));
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
	auto opaque = std::make_shared<DebugMesh>();
	opaque->SetMeshData(std::make_shared<MeshData>());
	auto xray = std::make_shared<DebugMesh>();
	xray->SetMeshData(std::make_shared<MeshData>());
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
// SceneController -> ObjectID::SetVisible. Every other consumer of a scene pool
// (GeometryPass, ShadowDepthPass, UploadManager) skips an object when either
// flag is clear, and the overlay has to do the same here: `DebugDrawList` has no
// per-primitive enable bit, so an invisible object must simply not be flattened.
//
// The rebuild that follows a toggle is already wired -- SceneController enqueues
// RenderResetEvent, whose handler calls MarkDirty() -- so these tests pin the
// half that was missing: the filter itself.

/// @test A hidden DebugLine contributes no segments.
TEST(DebugDrawBuilder, Visibility_ViewportHiddenLineIsSkipped)
{
	auto line = MakeLine(3, false);
	line->SetVisible(false, true);

	Scene scene;
	scene.UseDebugLine(line);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	EXPECT_TRUE(b.List().segments.empty());
	EXPECT_TRUE(b.List().Empty());
}

/**
 * @test Render visibility hides the overlay too, matching every other pass.
 *
 * The overlay is editor-only, so `is_viewport` is the flag that obviously
 * applies; `is_rendered` is honoured as well because every existing consumer
 * treats the two as an AND, and a gizmo that survived one toggle but not the
 * other would make the Outliner's two buttons mean different things per row.
 */
TEST(DebugDrawBuilder, Visibility_RenderHiddenLineIsSkipped)
{
	auto line = MakeLine(3, false);
	line->SetVisible(true, false);

	Scene scene;
	scene.UseDebugLine(line);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	EXPECT_TRUE(b.List().segments.empty());
}

/// @test Hiding points and wire meshes works the same way.
TEST(DebugDrawBuilder, Visibility_HiddenPointsAndWireMeshAreSkipped)
{
	auto pts = MakePoints(4, false);
	pts->SetVisible(false, true);

	auto mesh = std::make_shared<DebugMesh>();
	mesh->SetMeshData(std::make_shared<MeshData>());
	mesh->SetVisible(false, true);

	Scene scene;
	scene.UseDebugPoints(pts);
	scene.UseDebugMesh(mesh);

	DebugDrawBuilder b;
	b.Rebuild(scene);

	EXPECT_TRUE(b.List().points.empty());
	EXPECT_TRUE(b.List().wireMeshes.empty());
	EXPECT_TRUE(b.List().Empty());
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
	auto hiddenOpaque = MakeLine(2, false);  // 2 depth-tested segments, hidden
	hiddenOpaque->SetVisible(false, true);

	Scene scene;
	scene.UseDebugLine(hiddenOpaque);
	scene.UseDebugLine(MakeLine(3, false));  // 3 depth-tested
	scene.UseDebugLine(MakeLine(1, true));   // 1 x-ray

	DebugDrawBuilder b;
	b.Rebuild(scene);

	const DebugDrawList& l = b.List();
	ASSERT_EQ(l.segments.size(), 4u) << "The hidden line's 2 segments are gone";
	EXPECT_EQ(l.xraySegmentStart, 3u) << "Boundary follows the surviving segments";
	for (uint32_t i = 0; i < l.xraySegmentStart; ++i)
		EXPECT_FALSE(Has(l.segments[i].flags, DebugFlag::XRay));
	for (size_t i = l.xraySegmentStart; i < l.segments.size(); ++i)
		EXPECT_TRUE(Has(l.segments[i].flags, DebugFlag::XRay));
}

/**
 * @test Un-hiding brings the geometry back on the next rebuild.
 *
 * The visible path: a toggle enqueues RenderResetEvent, whose handler marks the
 * builder dirty, and Editor::Edit() rebuilds once. Without MarkDirty() the clean
 * builder would return early and the overlay would stay hidden until some other
 * mutation happened to dirty it.
 */
TEST(DebugDrawBuilder, Visibility_UnhidingRestoresGeometryOnNextRebuild)
{
	auto line = MakeLine(3, false);
	line->SetVisible(false, false);

	Scene scene;
	scene.UseDebugLine(line);

	DebugDrawBuilder b;
	b.Rebuild(scene);
	ASSERT_TRUE(b.List().segments.empty());
	const uint64_t hiddenRevision = b.List().revision;

	line->SetVisible(true, true);
	b.MarkDirty();
	b.Rebuild(scene);

	EXPECT_EQ(b.List().segments.size(), 3u);
	EXPECT_NE(b.List().revision, hiddenRevision)
	    << "DebugPass re-uploads only when the revision moves";
}
