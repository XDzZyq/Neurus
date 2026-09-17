/**
 * @file test_debug.cpp
 * @brief Unit tests for DebugLine and DebugPoints classes.
 *
 * TDD: RED (test written first) → GREEN (implementation verified).
 * All tests are pure CPU data storage - no GPU required.
 */

#include <gtest/gtest.h>

#include "scene/DebugLine.h"
#include "scene/DebugPoints.h"

using namespace neurus;

// ===========================================================================
// DebugLine Tests
// ===========================================================================

/**
 * @test DebugLine default-constructs with expected defaults.
 */
TEST(DebugLine, DefaultConstruction)
{
	DebugLine dl;

	// Inherits ObjectID with valid ID
	EXPECT_GE(dl.GetObjectID(), 0);

	// Object type is GO_DL
	EXPECT_EQ(dl.o_type, ObjectID::GOType::GO_DL);

	// Default color is white (fully opaque)
	EXPECT_EQ(dl.GetColor(), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

	// Default width is 1.0
	EXPECT_FLOAT_EQ(dl.GetWidth(), 1.0f);

	// Default opacity is 1.0
	EXPECT_FLOAT_EQ(dl.GetOpacity(), 1.0f);

	// Default flags
	EXPECT_FALSE(dl.GetStipple());
	EXPECT_FALSE(dl.GetSmooth());
	EXPECT_FALSE(dl.GetXRay());  // depth-tested by default, not always-on-top

	// No vertices initially
	EXPECT_EQ(dl.GetVertexCount(), 0);
	EXPECT_TRUE(dl.GetVertices().empty());
}

/**
 * @test DebugLine inherits Transform3D - can set position.
 */
TEST(DebugLine, InheritsTransform3D)
{
	DebugLine dl;
	dl.SetPosition(glm::vec3(10.0f, 20.0f, 30.0f));

	glm::mat4 model = dl.GetModelMatrix();
	EXPECT_FLOAT_EQ(model[3][0], 10.0f);
	EXPECT_FLOAT_EQ(model[3][1], 20.0f);
	EXPECT_FLOAT_EQ(model[3][2], 30.0f);

	// GetTransformPtr returns valid pointer to this Transform3D
	Transform* base = &dl;
	Transform3D* ptr = dynamic_cast<Transform3D*>(base->GetTransformPtr());
	EXPECT_NE(ptr, nullptr);
}

/**
 * @test PushDebugLine adds a pair of vertices (start/end).
 */
TEST(DebugLine, PushDebugLine_SingleSegment)
{
	DebugLine dl;
	dl.PushDebugLine(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 1.0f, 1.0f));

	EXPECT_EQ(dl.GetVertexCount(), 2);

	const auto& verts = dl.GetVertices();
	EXPECT_EQ(verts[0], glm::vec3(0.0f, 0.0f, 0.0f));
	EXPECT_EQ(verts[1], glm::vec3(1.0f, 1.0f, 1.0f));
}

/**
 * @test PushDebugLines appends multiple vertices.
 */
TEST(DebugLine, PushDebugLines_MultipleVertices)
{
	DebugLine dl;
	std::vector<glm::vec3> points = {
		{0.0f, 0.0f, 0.0f},
		{1.0f, 0.0f, 0.0f},
		{1.0f, 1.0f, 0.0f},
		{0.0f, 1.0f, 0.0f}
	};
	dl.PushDebugLines(points);

	EXPECT_EQ(dl.GetVertexCount(), 4);

	const auto& verts = dl.GetVertices();
	for (size_t i = 0; i < points.size(); ++i)
	{
		EXPECT_EQ(verts[i], points[i]);
	}
}

/**
 * @test Multiple PushDebugLine calls accumulate vertices.
 */
TEST(DebugLine, PushDebugLine_Accumulates)
{
	DebugLine dl;
	dl.PushDebugLine(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	dl.PushDebugLine(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 1.0f, 0.0f));

	EXPECT_EQ(dl.GetVertexCount(), 4);
}

/**
 * @test Color property can be set and retrieved.
 */
TEST(DebugLine, SetColor)
{
	DebugLine dl;
	glm::vec4 color(0.2f, 0.4f, 0.6f, 0.8f);
	dl.SetColor(color);
	EXPECT_EQ(dl.GetColor(), color);
}

/**
 * @test Width property can be set and retrieved.
 */
TEST(DebugLine, SetWidth)
{
	DebugLine dl;
	dl.SetWidth(3.0f);
	EXPECT_FLOAT_EQ(dl.GetWidth(), 3.0f);
}

/**
 * @test Opacity property can be set and retrieved.
 */
TEST(DebugLine, SetOpacity)
{
	DebugLine dl;
	dl.SetOpacity(0.5f);
	EXPECT_FLOAT_EQ(dl.GetOpacity(), 0.5f);
}

/**
 * @test Stipple flag can be set and retrieved.
 */
TEST(DebugLine, SetStipple)
{
	DebugLine dl;
	EXPECT_FALSE(dl.GetStipple());
	dl.SetStipple(true);
	EXPECT_TRUE(dl.GetStipple());
}

/**
 * @test Smooth flag can be set and retrieved.
 */
TEST(DebugLine, SetSmooth)
{
	DebugLine dl;
	EXPECT_FALSE(dl.GetSmooth());
	dl.SetSmooth(true);
	EXPECT_TRUE(dl.GetSmooth());
}

/**
 * @test ClearVertices removes all stored vertices.
 */
TEST(DebugLine, ClearVertices)
{
	DebugLine dl;
	dl.PushDebugLine(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 1.0f, 1.0f));
	EXPECT_EQ(dl.GetVertexCount(), 2);

	dl.ClearVertices();
	EXPECT_EQ(dl.GetVertexCount(), 0);
	EXPECT_TRUE(dl.GetVertices().empty());
}

/**
 * @test DebugLine is non-copyable (respects UID semantics).
 */
TEST(DebugLine, NonCopyable)
{
	EXPECT_FALSE(std::is_copy_constructible_v<DebugLine>);
	EXPECT_FALSE(std::is_copy_assignable_v<DebugLine>);
}

// ===========================================================================
// DebugPoints Tests
// ===========================================================================

/**
 * @test DebugPoints default-constructs with expected defaults.
 */
TEST(DebugPoints, DefaultConstruction)
{
	DebugPoints dp;

	// Inherits ObjectID with valid ID
	EXPECT_GE(dp.GetObjectID(), 0);

	// Object type is GO_DP
	EXPECT_EQ(dp.o_type, ObjectID::GOType::GO_DP);

	// Default point type is SQUARE
	EXPECT_EQ(dp.GetPointType(), DebugPoints::PointType::SQUARE);

	// Default color is white (fully opaque)
	EXPECT_EQ(dp.GetColor(), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

	// Default scale is 8.0: the default projection mode is screen space, so
	// scale is a pixel diameter, and a 1 px sprite is effectively invisible.
	EXPECT_FLOAT_EQ(dp.GetScale(), 8.0f);

	// Default opacity is 1.0
	EXPECT_FLOAT_EQ(dp.GetOpacity(), 1.0f);

	// Default projection mode is 0
	EXPECT_EQ(dp.GetProjectionMode(), 0);

	// Depth-tested by default, not always-on-top
	EXPECT_FALSE(dp.GetXRay());

	// No points initially
	EXPECT_EQ(dp.GetPointCount(), 0);
	EXPECT_TRUE(dp.GetPoints().empty());
}

/**
 * @test DebugPoints inherits Transform3D - can set position.
 */
TEST(DebugPoints, InheritsTransform3D)
{
	DebugPoints dp;
	dp.SetPosition(glm::vec3(5.0f, 10.0f, -3.0f));

	glm::mat4 model = dp.GetModelMatrix();
	EXPECT_FLOAT_EQ(model[3][0], 5.0f);
	EXPECT_FLOAT_EQ(model[3][1], 10.0f);
	EXPECT_FLOAT_EQ(model[3][2], -3.0f);

	Transform* base = &dp;
	Transform3D* ptr = dynamic_cast<Transform3D*>(base->GetTransformPtr());
	EXPECT_NE(ptr, nullptr);
}

/**
 * @test PushDebugPoint adds a single point.
 */
TEST(DebugPoints, PushDebugPoint_Single)
{
	DebugPoints dp;
	dp.PushDebugPoint(glm::vec3(1.0f, 2.0f, 3.0f));

	EXPECT_EQ(dp.GetPointCount(), 1);

	const auto& pts = dp.GetPoints();
	EXPECT_EQ(pts[0], glm::vec3(1.0f, 2.0f, 3.0f));
}

/**
 * @test PushDebugPoints appends multiple points.
 */
TEST(DebugPoints, PushDebugPoints_Multiple)
{
	DebugPoints dp;
	std::vector<glm::vec3> points = {
		{0.0f, 0.0f, 0.0f},
		{1.0f, 0.0f, 0.0f},
		{0.0f, 1.0f, 0.0f}
	};
	dp.PushDebugPoints(points);

	EXPECT_EQ(dp.GetPointCount(), 3);

	const auto& pts = dp.GetPoints();
	for (size_t i = 0; i < points.size(); ++i)
	{
		EXPECT_EQ(pts[i], points[i]);
	}
}

/**
 * @test Multiple PushDebugPoint calls accumulate points.
 */
TEST(DebugPoints, PushDebugPoint_Accumulates)
{
	DebugPoints dp;
	dp.PushDebugPoint(glm::vec3(0.0f, 0.0f, 0.0f));
	dp.PushDebugPoint(glm::vec3(1.0f, 1.0f, 1.0f));
	dp.PushDebugPoint(glm::vec3(2.0f, 2.0f, 2.0f));

	EXPECT_EQ(dp.GetPointCount(), 3);
}

/**
 * @test PointType property can be set and retrieved for all types.
 */
TEST(DebugPoints, SetPointType)
{
	DebugPoints dp;

	dp.SetPointType(DebugPoints::PointType::SQUARE);
	EXPECT_EQ(dp.GetPointType(), DebugPoints::PointType::SQUARE);

	dp.SetPointType(DebugPoints::PointType::RHOMBUS);
	EXPECT_EQ(dp.GetPointType(), DebugPoints::PointType::RHOMBUS);

	dp.SetPointType(DebugPoints::PointType::CIR);
	EXPECT_EQ(dp.GetPointType(), DebugPoints::PointType::CIR);

	dp.SetPointType(DebugPoints::PointType::CUBE);
	EXPECT_EQ(dp.GetPointType(), DebugPoints::PointType::CUBE);
}

/**
 * @test Color property can be set and retrieved.
 */
TEST(DebugPoints, SetColor)
{
	DebugPoints dp;
	glm::vec4 color(1.0f, 0.0f, 0.0f, 0.5f);
	dp.SetColor(color);
	EXPECT_EQ(dp.GetColor(), color);
}

/**
 * @test Scale property can be set and retrieved.
 */
TEST(DebugPoints, SetScale)
{
	DebugPoints dp;
	dp.SetScale(5.0f);
	EXPECT_FLOAT_EQ(dp.GetScale(), 5.0f);
}

/**
 * @test Opacity property can be set and retrieved.
 */
TEST(DebugPoints, SetOpacity)
{
	DebugPoints dp;
	dp.SetOpacity(0.25f);
	EXPECT_FLOAT_EQ(dp.GetOpacity(), 0.25f);
}

/**
 * @test ProjectionMode property can be set and retrieved.
 */
TEST(DebugPoints, SetProjectionMode)
{
	DebugPoints dp;
	dp.SetProjectionMode(1);
	EXPECT_EQ(dp.GetProjectionMode(), 1);
}

/**
 * @test ClearPoints removes all stored points.
 */
TEST(DebugPoints, ClearPoints)
{
	DebugPoints dp;
	dp.PushDebugPoint(glm::vec3(1.0f, 2.0f, 3.0f));
	dp.PushDebugPoint(glm::vec3(4.0f, 5.0f, 6.0f));
	EXPECT_EQ(dp.GetPointCount(), 2);

	dp.ClearPoints();
	EXPECT_EQ(dp.GetPointCount(), 0);
	EXPECT_TRUE(dp.GetPoints().empty());
}

/**
 * @test DebugPoints is non-copyable (respects UID semantics).
 */
TEST(DebugPoints, NonCopyable)
{
	EXPECT_FALSE(std::is_copy_constructible_v<DebugPoints>);
	EXPECT_FALSE(std::is_copy_assignable_v<DebugPoints>);
}
