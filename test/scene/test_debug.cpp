/**
 * @file test_debug.cpp
 * @brief Unit tests for DebugLine and DebugPoints classes.
 *
 * TDD: RED (test written first) → GREEN (implementation verified).
 * All tests are pure CPU data storage - no GPU required.
 */

#include <gtest/gtest.h>

#include "scene/DebugLine.hpp"
#include "scene/DebugPoints.hpp"

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
	EXPECT_EQ(dl.GetObjectID(), -1);

	// Object type is GO_DL
	EXPECT_EQ(dl.o_type, ObjectID::GOType::GO_DL);

	// Default color is white (fully opaque)
	EXPECT_EQ(dl.GetColor(), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

	// Default width is 1.0
	EXPECT_FLOAT_EQ(dl.GetWidth(), 1.0f);

	// Default opacity is 1.0
	EXPECT_FLOAT_EQ(dl.GetOpacity(), 1.0f);

	// Default flags. There is no smooth flag to check: antialiasing is derived
	// from the line style by DebugDrawBuilder, never stored on the object.
	EXPECT_FALSE(dl.GetStipple());
	EXPECT_FALSE(dl.GetXRay());  // depth-tested by default, not always-on-top

	// No vertices initially
	EXPECT_EQ(dl.GetVertexCount(), 0);
	EXPECT_TRUE(dl.GetVertices().empty());
}

TEST(DebugLine, AppendsSegmentsInOrderUntilCleared)
{
	DebugLine lines;
	const glm::vec3 a(1, 0, 0), b(0, 1, 0), c(0, 0, 1);
	lines.PushDebugLine(a, b);
	lines.PushDebugLine(b, c);
	lines.PushDebugLines({c, a});
	const std::vector<glm::vec3> expected{a, b, b, c, c, a};
	EXPECT_EQ(lines.GetVertices(), expected);
	EXPECT_EQ(lines.GetVertexCount(), expected.size());
	lines.ClearVertices();
	EXPECT_TRUE(lines.GetVertices().empty());
	EXPECT_EQ(lines.GetVertexCount(), 0);
}

TEST(DebugLine, UpdatesDrawingStyle)
{
	DebugLine dl;

	glm::vec4 color(0.2f, 0.4f, 0.6f, 0.8f);
	dl.SetColor(color);
	EXPECT_EQ(dl.GetColor(), color);

	dl.SetOpacity(0.5f);
	EXPECT_FLOAT_EQ(dl.GetOpacity(), 0.5f);

	dl.SetWidth(3.0f);
	EXPECT_FLOAT_EQ(dl.GetWidth(), 3.0f);

	EXPECT_FALSE(dl.GetStipple());
	dl.SetStipple(true);
	EXPECT_TRUE(dl.GetStipple());
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
	EXPECT_EQ(dp.GetObjectID(), -1);

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

TEST(DebugPoints, AppendsPointsInOrderUntilCleared)
{
	DebugPoints points;
	const glm::vec3 a(1, 0, 0), b(0, 1, 0), c(0, 0, 1);
	points.PushDebugPoint(a);
	points.PushDebugPoint(b);
	points.PushDebugPoints({c, a});
	const std::vector<glm::vec3> expected{a, b, c, a};
	EXPECT_EQ(points.GetPoints(), expected);
	EXPECT_EQ(points.GetPointCount(), expected.size());
	points.ClearPoints();
	EXPECT_TRUE(points.GetPoints().empty());
	EXPECT_EQ(points.GetPointCount(), 0);
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

TEST(DebugPoints, UpdatesDrawingStyle)
{
	DebugPoints dp;

	glm::vec4 color(1.0f, 0.0f, 0.0f, 0.5f);
	dp.SetColor(color);
	EXPECT_EQ(dp.GetColor(), color);

	dp.SetOpacity(0.25f);
	EXPECT_FLOAT_EQ(dp.GetOpacity(), 0.25f);

	dp.SetScale(5.0f);
	EXPECT_FLOAT_EQ(dp.GetScale(), 5.0f);

	dp.SetProjectionMode(1);
	EXPECT_EQ(dp.GetProjectionMode(), 1);
}
