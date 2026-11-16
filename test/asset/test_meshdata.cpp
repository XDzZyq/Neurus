#include <gtest/gtest.h>

#include "asset/data/MeshData.hpp"

#include <cmath>
#include <sstream>

using namespace neurus;

// ---------------------------------------------------------------------------
// Helper: build an OBJ string from multiple lines
// ---------------------------------------------------------------------------

static std::string MakeObj(std::initializer_list<const char*> lines)
{
	std::ostringstream oss;
	for (auto& line : lines)
	{
		oss << line << "\n";
	}
	return oss.str();
}

// ---------------------------------------------------------------------------
// 1. Simple triangle - all components (pos, uv, norm)
// ---------------------------------------------------------------------------

TEST(MeshDataTest, Triangle_AllComponents)
{
	const std::string obj = MakeObj({
		"v 0.0 0.0 0.0",
		"v 1.0 0.0 0.0",
		"v 0.0 1.0 0.0",
		"vt 0.0 0.0",
		"vt 1.0 0.0",
		"vt 0.0 1.0",
		"vn 0.0 0.0 1.0",
		"vn 0.0 0.0 1.0",
		"vn 0.0 0.0 1.0",
		"f 1/1/1 2/2/2 3/3/3"
	});

	MeshData mesh;
	ASSERT_TRUE(mesh.LoadObjFromString(obj));

	EXPECT_EQ(mesh.GetVertexCount(), 3u);
	EXPECT_EQ(mesh.GetIndexCount(), 3u);

	const auto& data = mesh.GetMeshData();
	ASSERT_EQ(data.dataArray.size(), 3u * 14u);
	ASSERT_EQ(data.indexArray.size(), 3u);

	// Indices should be 0,1,2 for a simple triangle
	EXPECT_EQ(data.indexArray[0], 0u);
	EXPECT_EQ(data.indexArray[1], 1u);
	EXPECT_EQ(data.indexArray[2], 2u);

	// Check first vertex: position (0,0,0)
	EXPECT_FLOAT_EQ(data.dataArray[0], 0.0f);
	EXPECT_FLOAT_EQ(data.dataArray[1], 0.0f);
	EXPECT_FLOAT_EQ(data.dataArray[2], 0.0f);

	// UV (0,0)
	EXPECT_FLOAT_EQ(data.dataArray[6], 0.0f);
	EXPECT_FLOAT_EQ(data.dataArray[7], 0.0f);

	// Normal (0,0,1)
	EXPECT_FLOAT_EQ(data.dataArray[3], 0.0f);
	EXPECT_FLOAT_EQ(data.dataArray[4], 0.0f);
	EXPECT_FLOAT_EQ(data.dataArray[5], 1.0f);

	// Check second vertex: position (1,0,0)
	EXPECT_FLOAT_EQ(data.dataArray[14 + 0], 1.0f);
	EXPECT_FLOAT_EQ(data.dataArray[14 + 1], 0.0f);
	EXPECT_FLOAT_EQ(data.dataArray[14 + 2], 0.0f);

	// Center should be roughly (0.33, 0.33, 0.0)
	glm::vec3 center = mesh.GetMeshCenter();
	EXPECT_NEAR(center.x, 0.33333f, 0.01f);
	EXPECT_NEAR(center.y, 0.33333f, 0.01f);
	EXPECT_NEAR(center.z, 0.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// 2. Triangle without UVs - format: f v//vn
// ---------------------------------------------------------------------------

TEST(MeshDataTest, Triangle_NoUVs)
{
	const std::string obj = MakeObj({
		"v 0.0 0.0 0.0",
		"v 1.0 0.0 0.0",
		"v 0.0 1.0 0.0",
		"vn 0.0 0.0 1.0",
		"f 1//1 2//1 3//1"
	});

	MeshData mesh;
	ASSERT_TRUE(mesh.LoadObjFromString(obj));

	EXPECT_EQ(mesh.GetVertexCount(), 3u);
	EXPECT_EQ(mesh.GetIndexCount(), 3u);

	const auto& data = mesh.GetMeshData();
	ASSERT_EQ(data.dataArray.size(), 3u * 14u);

	// UVs should be zero for all vertices
	for (int v = 0; v < 3; ++v)
	{
		EXPECT_FLOAT_EQ(data.dataArray[v * 14 + 6], 0.0f);
		EXPECT_FLOAT_EQ(data.dataArray[v * 14 + 7], 0.0f);
	}
}

// ---------------------------------------------------------------------------
// 3. Triangle without normals - format: f v/vt
// ---------------------------------------------------------------------------

TEST(MeshDataTest, Triangle_NoNormals)
{
	const std::string obj = MakeObj({
		"v 0.0 0.0 0.0",
		"v 1.0 0.0 0.0",
		"v 0.0 1.0 0.0",
		"vt 0.0 0.0",
		"vt 1.0 0.0",
		"vt 0.0 1.0",
		"f 1/1 2/2 3/3"
	});

	MeshData mesh;
	ASSERT_TRUE(mesh.LoadObjFromString(obj));

	EXPECT_EQ(mesh.GetVertexCount(), 3u);
	EXPECT_EQ(mesh.GetIndexCount(), 3u);

	const auto& data = mesh.GetMeshData();

	// Normals should be computed: for this triangle (z-up), normal ≈ (0,0,1)
	// Cross product: (1,0,0) x (0,1,0) = (0,0,1) for the 2nd and 3rd vertices vs 1st
	for (int v = 0; v < 3; ++v)
	{
		EXPECT_NEAR(data.dataArray[v * 14 + 3], 0.0f, 0.01f);
		EXPECT_NEAR(data.dataArray[v * 14 + 4], 0.0f, 0.01f);
		EXPECT_NEAR(data.dataArray[v * 14 + 5], 1.0f, 0.01f);
	}
}

// ---------------------------------------------------------------------------
// 4. Triangle with only positions - format: f v
// ---------------------------------------------------------------------------

TEST(MeshDataTest, Triangle_OnlyPositions)
{
	const std::string obj = MakeObj({
		"v 0.0 0.0 0.0",
		"v 1.0 0.0 0.0",
		"v 0.0 1.0 0.0",
		"f 1 2 3"
	});

	MeshData mesh;
	ASSERT_TRUE(mesh.LoadObjFromString(obj));

	EXPECT_EQ(mesh.GetVertexCount(), 3u);
	EXPECT_EQ(mesh.GetIndexCount(), 3u);

	const auto& data = mesh.GetMeshData();
	ASSERT_EQ(data.dataArray.size(), 3u * 14u);

	// Normals should be computed
	for (int v = 0; v < 3; ++v)
	{
		float nz = data.dataArray[v * 14 + 5];
		EXPECT_NEAR(std::abs(nz), 1.0f, 0.01f);
	}
}

// ---------------------------------------------------------------------------
// 5. Quad face - should triangulate into 2 triangles (6 indices, 4 unique verts)
// ---------------------------------------------------------------------------

TEST(MeshDataTest, Quad_Triangulation)
{
	const std::string obj = MakeObj({
		"v -1.0 -1.0 0.0",
		"v  1.0 -1.0 0.0",
		"v  1.0  1.0 0.0",
		"v -1.0  1.0 0.0",
		"vt 0.0 0.0",
		"vt 1.0 0.0",
		"vt 1.0 1.0",
		"vt 0.0 1.0",
		"vn 0.0 0.0 1.0",
		"f 1/1/1 2/2/1 3/3/1 4/4/1"
	});

	MeshData mesh;
	ASSERT_TRUE(mesh.LoadObjFromString(obj));

	// 4 unique vertices, 2 triangles = 6 indices
	EXPECT_EQ(mesh.GetVertexCount(), 4u);
	EXPECT_EQ(mesh.GetIndexCount(), 6u);

	const auto& data = mesh.GetMeshData();
	ASSERT_EQ(data.dataArray.size(), 4u * 14u);
	ASSERT_EQ(data.indexArray.size(), 6u);

	// First triangle: 0,1,2
	// Second triangle: 0,2,3  (fan triangulation)
	EXPECT_EQ(data.indexArray[3], 0u);
	EXPECT_EQ(data.indexArray[4], 2u);
	EXPECT_EQ(data.indexArray[5], 3u);
}

// ---------------------------------------------------------------------------
// 6. Comments and empty lines - should be ignored
// ---------------------------------------------------------------------------

TEST(MeshDataTest, CommentsAndEmptyLines)
{
	const std::string obj = MakeObj({
		"# This is a comment",
		"",
		"v 0.0 0.0 0.0",
		"# Another comment",
		"v 1.0 0.0 0.0",
		"",
		"v 0.0 1.0 0.0",
		"vn 0.0 0.0 1.0",
		"f 1//1 2//1 3//1",
		""
	});

	MeshData mesh;
	ASSERT_TRUE(mesh.LoadObjFromString(obj));

	EXPECT_EQ(mesh.GetVertexCount(), 3u);
	EXPECT_EQ(mesh.GetIndexCount(), 3u);
}

// ---------------------------------------------------------------------------
// 7. Negative / relative indices - -1 = last, -2 = second-to-last, etc.
// ---------------------------------------------------------------------------

TEST(MeshDataTest, NegativeIndices)
{
	const std::string obj = MakeObj({
		"v 0.0 0.0 0.0",
		"v 1.0 0.0 0.0",
		"v 0.0 1.0 0.0",
		"vn 0.0 0.0 1.0",
		"f -3//-1 -2//-1 -1//-1"
	});

	MeshData mesh;
	ASSERT_TRUE(mesh.LoadObjFromString(obj));

	EXPECT_EQ(mesh.GetVertexCount(), 3u);
	EXPECT_EQ(mesh.GetIndexCount(), 3u);

	const auto& data = mesh.GetMeshData();

	// -3 maps to position 1 (0,0,0)
	EXPECT_FLOAT_EQ(data.dataArray[0], 0.0f);
	EXPECT_FLOAT_EQ(data.dataArray[1], 0.0f);
	EXPECT_FLOAT_EQ(data.dataArray[2], 0.0f);

	// -2 maps to position 2 (1,0,0)
	EXPECT_FLOAT_EQ(data.dataArray[14 + 0], 1.0f);
	EXPECT_FLOAT_EQ(data.dataArray[14 + 1], 0.0f);
	EXPECT_FLOAT_EQ(data.dataArray[14 + 2], 0.0f);

	// -1 maps to position 3 (0,1,0)
	EXPECT_FLOAT_EQ(data.dataArray[28 + 0], 0.0f);
	EXPECT_FLOAT_EQ(data.dataArray[28 + 1], 1.0f);
	EXPECT_FLOAT_EQ(data.dataArray[28 + 2], 0.0f);
}

// ---------------------------------------------------------------------------
// 8. Vertex deduplication - reused vertices should share index
// ---------------------------------------------------------------------------

TEST(MeshDataTest, VertexDeduplication)
{
	// Two identical triangles sharing a vertex
	const std::string obj = MakeObj({
		"v 0.0 0.0 0.0",
		"v 1.0 0.0 0.0",
		"v 0.0 1.0 0.0",
		"v 1.0 1.0 0.0",
		"vn 0.0 0.0 1.0",
		"f 1//1 2//1 3//1",
		"f 2//1 4//1 3//1"
	});

	MeshData mesh;
	ASSERT_TRUE(mesh.LoadObjFromString(obj));

	// 4 unique position+normal combos, 2 triangles = 6 indices
	EXPECT_EQ(mesh.GetVertexCount(), 4u);
	EXPECT_EQ(mesh.GetIndexCount(), 6u);

	const auto& data = mesh.GetMeshData();
	ASSERT_EQ(data.dataArray.size(), 4u * 14u);

	// Vertex at (0,0,0) should appear exactly once in data
	int countZero = 0;
	for (size_t i = 0; i < data.dataArray.size(); i += 14)
	{
		if (data.dataArray[i] == 0.0f && data.dataArray[i + 1] == 0.0f && data.dataArray[i + 2] == 0.0f)
			countZero++;
	}
	EXPECT_EQ(countZero, 1);
}

// ---------------------------------------------------------------------------
// 9. Object name - parsed from 'o' line
// ---------------------------------------------------------------------------

TEST(MeshDataTest, ObjectName)
{
	const std::string obj = MakeObj({
		"o TestCube",
		"v 0.0 0.0 0.0",
		"v 1.0 0.0 0.0",
		"v 0.0 1.0 0.0",
		"vn 0.0 0.0 1.0",
		"f 1//1 2//1 3//1"
	});

	MeshData mesh;
	ASSERT_TRUE(mesh.LoadObjFromString(obj));

	EXPECT_EQ(mesh.GetMeshName(), "TestCube");
}

// ---------------------------------------------------------------------------
// 10. Tangent computation verification
// ---------------------------------------------------------------------------

TEST(MeshDataTest, TangentComputation)
{
	const std::string obj = MakeObj({
		"v 0.0 0.0 0.0",
		"v 1.0 0.0 0.0",
		"v 0.0 1.0 0.0",
		"vt 0.0 0.0",
		"vt 1.0 0.0",
		"vt 0.0 1.0",
		"vn 0.0 0.0 1.0",
		"f 1/1/1 2/2/1 3/3/1"
	});

	MeshData mesh;
	ASSERT_TRUE(mesh.LoadObjFromString(obj));

	const auto& data = mesh.GetMeshData();

	// Tangent should be roughly (1,0,0) for this UV layout
	for (int v = 0; v < 3; ++v)
	{
		float tx = data.dataArray[v * 14 + 8];
		float ty = data.dataArray[v * 14 + 9];
		float tz = data.dataArray[v * 14 + 10];

		// Tangent should be along +X direction
		EXPECT_NEAR(tx, 1.0f, 0.01f);
		EXPECT_NEAR(ty, 0.0f, 0.01f);
		EXPECT_NEAR(tz, 0.0f, 0.01f);

		// Bitangent should be roughly (0,1,0) since normal is (0,0,1) and tangent is (1,0,0)
		float bx = data.dataArray[v * 14 + 11];
		float by = data.dataArray[v * 14 + 12];
		float bz = data.dataArray[v * 14 + 13];
		EXPECT_NEAR(bx, 0.0f, 0.01f);
		EXPECT_NEAR(by, 1.0f, 0.01f);
		EXPECT_NEAR(bz, 0.0f, 0.01f);
	}
}
