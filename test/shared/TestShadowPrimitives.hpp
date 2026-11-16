/**
 * @file TestShadowPrimitives.hpp
 * @brief Shared procedural cube/plane OBJ geometry for shadow-map test scenes.
 *
 * Consolidates the cube+plane OBJ strings and MeshData construction that
 * were previously duplicated verbatim between TestSimpleShadow.hpp and
 * TestMultiShadow.hpp.
 */

#pragma once

#include "asset/data/MeshData.hpp"
#include "core/Log.hpp"

#include <memory>

namespace neurus {
namespace test {

/**
 * @brief Unit cube OBJ source, centred at origin, covering [-0.5, +0.5]^3.
 *        8 unique vertices, 12 triangles (36 indices).
 */
inline constexpr const char* kShadowTestCubeObj = R"OBJ(
v -0.5 -0.5 -0.5
v 0.5 -0.5 -0.5
v 0.5 -0.5 0.5
v -0.5 -0.5 0.5
v -0.5 0.5 -0.5
v 0.5 0.5 -0.5
v 0.5 0.5 0.5
v -0.5 0.5 0.5

f 1 2 3 4
f 5 8 7 6
f 1 5 6 2
f 4 3 7 8
f 1 4 8 5
f 2 6 7 3
)OBJ";

/**
 * @brief Ground-plane OBJ source: large quad at z=0, [-10,10] in XY, facing +Z.
 *        4 vertices, 2 triangles (6 indices).
 */
inline constexpr const char* kShadowTestPlaneObj = R"OBJ(
v -10 -10 0
v 10 -10 0
v 10 10 0
v -10 10 0

f 1 2 3 4
)OBJ";

/**
 * @brief Loads the shared unit-cube OBJ into a new MeshData.
 * @param callerTag Short tag used in the error log if parsing fails (e.g. "[LoadSimpleShadow]").
 * @return Non-null MeshData on success; caller should check LoadObjFromString succeeded
 *         via the returned validity if strict checking is needed (this helper logs on failure).
 */
inline std::shared_ptr<MeshData> MakeShadowTestCubeMeshData(const char* callerTag)
{
	auto meshData = std::make_shared<MeshData>();
	if (!meshData->LoadObjFromString(kShadowTestCubeObj))
	{
		NEURUS_ERR(callerTag << " Failed to parse cube OBJ string");
	}
	return meshData;
}

/**
 * @brief Loads the shared ground-plane OBJ into a new MeshData.
 * @param callerTag Short tag used in the error log if parsing fails (e.g. "[LoadSimpleShadow]").
 */
inline std::shared_ptr<MeshData> MakeShadowTestPlaneMeshData(const char* callerTag)
{
	auto meshData = std::make_shared<MeshData>();
	if (!meshData->LoadObjFromString(kShadowTestPlaneObj))
	{
		NEURUS_ERR(callerTag << " Failed to parse plane OBJ string");
	}
	return meshData;
}

} // namespace test
} // namespace neurus
