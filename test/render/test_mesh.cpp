/**
 * @file test_mesh.cpp
 * @brief GPU tests for RenderCache mesh GPU insertion / caching lifecycle.
 *
 * Validates that:
 *   - UseMeshGPU stores a MeshGPU and GetMeshGPU returns it
 *   - GetMeshGPU returns the same MeshGPU on subsequent calls (caching)
 *   - RemoveMeshGPU destroys GPU buffers
 *
 * @note Requires a Vulkan 1.4-capable GPU. Skipped in CI without GPU.
 */



#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "shared/TestVulkanShared.h"

#include "scene/Mesh.h"
#include "asset/data/MeshData.h"
#include "render/RenderCache.h"
#include "render/resources/MeshGPU.h"
#include "render/buffers/VertexBuffer.h"
#include "render/buffers/IndexBuffer.h"

using namespace neurus;

// ---------------------------------------------------------------------------
// In-memory OBJ: a single triangle with normals
// ---------------------------------------------------------------------------

namespace
{
	const char* kTriangleObj =
		"v 0.0 0.0 0.0\n"
		"v 1.0 0.0 0.0\n"
		"v 0.0 1.0 0.0\n"
		"vn 0.0 0.0 1.0\n"
		"f 1//1 2//1 3//1\n";

	/**
	 * @brief Creates a MeshGPU from MeshData by manually constructing
	 *        VertexBuffer and IndexBuffer (was lazy-created by GetMeshGPU).
	 */
	MeshGPU CreateTestMeshGPU(const vk::raii::Device& device,
	                          const vk::raii::PhysicalDevice& physicalDevice,
	                          neurus::SharedQueue& queue,
	                          uint32_t queueFamilyIndex,
	                          const MeshData& meshData,
	                          int objectId)
	{
		const auto& rawMesh = meshData.GetMeshData();
		const size_t vertexCount = rawMesh.dataArray.size() / 14;
		const size_t indexCount = rawMesh.indexArray.size();

		if (vertexCount == 0 || indexCount == 0)
		{
			return MeshGPU{};
		}

		// Strip from 14 floats → 8 floats (pos+normal+uv)
		constexpr size_t kSrcStride = 14;
		constexpr size_t kDstStride = 8;
		std::vector<float> stripped(vertexCount * kDstStride);
		for (size_t i = 0; i < vertexCount; ++i)
		{
			std::memcpy(&stripped[i * kDstStride],
			            &rawMesh.dataArray[i * kSrcStride],
			            kDstStride * sizeof(float));
		}

		const uint32_t stride = static_cast<uint32_t>(kDstStride * sizeof(float));
		const vk::DeviceSize vSize = stripped.size() * sizeof(float);
		const vk::DeviceSize iSize = indexCount * sizeof(uint32_t);

		MeshGPU gpu;
		gpu.vertexBuffer = std::make_unique<VertexBuffer>(
			device, physicalDevice, queue, queueFamilyIndex,
			stripped.data(), vSize, stride,
			static_cast<uint32_t>(vertexCount),
			("TestMesh_VBO_" + std::to_string(objectId)).c_str());

		gpu.indexBuffer = std::make_unique<IndexBuffer>(
			device, physicalDevice, queue, queueFamilyIndex,
			rawMesh.indexArray.data(), iSize,
			static_cast<uint32_t>(indexCount),
			("TestMesh_IBO_" + std::to_string(objectId)).c_str());

		gpu.vertexCount = static_cast<uint32_t>(vertexCount);
		gpu.indexCount = static_cast<uint32_t>(indexCount);

		return gpu;
	}
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class MeshRenderTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;

		m_cache = std::make_unique<RenderCache>(*m_device, PhysicalDevice());
		m_cache->SetLightingCache(std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), *m_queue, m_graphicsQueueFamily));
	}

	void TearDown() override
	{
		m_cache.reset();
		VulkanTestShared::TearDown();
	}

	std::unique_ptr<RenderCache> m_cache;
};

// ---------------------------------------------------------------------------
// UseMeshGPU_Stores
// ---------------------------------------------------------------------------

/**
 * @brief After UseMeshGPU, GetMeshGPU returns a non-null pointer with
 *        correct buffer and index count.
 */
TEST_F(MeshRenderTest, UseMeshGPU_Stores)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	auto meshData = std::make_shared<MeshData>();
	ASSERT_TRUE(meshData->LoadObjFromString(kTriangleObj));

	constexpr int kTestObjectId = 42;
	auto gpu = CreateTestMeshGPU(*m_device, PhysicalDevice(),
	                              *m_queue, m_graphicsQueueFamily,
	                              *meshData, kTestObjectId);
	EXPECT_NE(gpu.vertexBuffer, nullptr);
	EXPECT_NE(gpu.indexBuffer, nullptr);
	EXPECT_GT(gpu.indexCount, 0u);

	m_cache->UseMeshGPU(kTestObjectId, std::move(gpu));

	MeshGPU* retrieved = m_cache->GetMeshGPU(kTestObjectId);
	ASSERT_NE(retrieved, nullptr);
	EXPECT_NE(retrieved->vertexBuffer, nullptr);
	EXPECT_NE(retrieved->indexBuffer, nullptr);
	EXPECT_EQ(retrieved->indexCount, gpu.indexCount); // moved-from; use captured
}

// ---------------------------------------------------------------------------
// GetMeshGPU_ReturnsCached
// ---------------------------------------------------------------------------

/**
 * @brief Calling GetMeshGPU twice returns the same pointer (cached).
 */
TEST_F(MeshRenderTest, GetMeshGPU_ReturnsCached)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	auto meshData = std::make_shared<MeshData>();
	ASSERT_TRUE(meshData->LoadObjFromString(kTriangleObj));

	constexpr int kTestObjectId = 99;
	auto gpu = CreateTestMeshGPU(*m_device, PhysicalDevice(),
	                              *m_queue, m_graphicsQueueFamily,
	                              *meshData, kTestObjectId);
	m_cache->UseMeshGPU(kTestObjectId, std::move(gpu));

	MeshGPU* first  = m_cache->GetMeshGPU(kTestObjectId);
	MeshGPU* second = m_cache->GetMeshGPU(kTestObjectId);

	ASSERT_NE(first, nullptr);
	EXPECT_EQ(first, second);
	EXPECT_NE(first->vertexBuffer, nullptr);
	EXPECT_NE(first->indexBuffer, nullptr);
}

// ---------------------------------------------------------------------------
// RemoveMeshGPU_Destroys
// ---------------------------------------------------------------------------

/**
 * @brief After RemoveMeshGPU, GetMeshGPU returns nullptr.
 */
TEST_F(MeshRenderTest, RemoveMeshGPU_Destroys)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	auto meshData = std::make_shared<MeshData>();
	ASSERT_TRUE(meshData->LoadObjFromString(kTriangleObj));

	constexpr int kTestObjectId = 77;

	// Insert
	auto gpu = CreateTestMeshGPU(*m_device, PhysicalDevice(),
	                              *m_queue, m_graphicsQueueFamily,
	                              *meshData, kTestObjectId);
	m_cache->UseMeshGPU(kTestObjectId, std::move(gpu));

	MeshGPU* created = m_cache->GetMeshGPU(kTestObjectId);
	ASSERT_NE(created, nullptr);
	EXPECT_NE(created->vertexBuffer, nullptr);

	// Remove
	m_cache->RemoveMeshGPU(kTestObjectId);
	EXPECT_EQ(m_cache->GetMeshGPU(kTestObjectId), nullptr);

	// Re-insert a new one
	auto gpu2 = CreateTestMeshGPU(*m_device, PhysicalDevice(),
	                               *m_queue, m_graphicsQueueFamily,
	                               *meshData, kTestObjectId);
	m_cache->UseMeshGPU(kTestObjectId, std::move(gpu2));

	MeshGPU* recreated = m_cache->GetMeshGPU(kTestObjectId);
	ASSERT_NE(recreated, nullptr);
	EXPECT_NE(recreated->vertexBuffer, nullptr);
	// Verify re-inserted entry has the expected vertex count
	EXPECT_EQ(recreated->vertexCount, 3u);
	// The created pointer is now dangling (erased + re-inserted);
	// verify the new entry is retrievable by its key instead
	EXPECT_EQ(m_cache->GetMeshGPU(kTestObjectId), recreated);
}

// ---------------------------------------------------------------------------
// UseMeshGPU_EmptyMeshData
// ---------------------------------------------------------------------------

/**
 * @brief UseMeshGPU with empty MeshGPU (null buffers, zero counts)
 *        is stored and retrievable.
 */
TEST_F(MeshRenderTest, UseMeshGPU_EmptyMeshData)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	constexpr int kTestObjectId = 0;
	MeshGPU emptyGpu;
	m_cache->UseMeshGPU(kTestObjectId, std::move(emptyGpu));

	MeshGPU* retrieved = m_cache->GetMeshGPU(kTestObjectId);
	ASSERT_NE(retrieved, nullptr);
	EXPECT_EQ(retrieved->vertexBuffer, nullptr);
	EXPECT_EQ(retrieved->indexBuffer, nullptr);
	EXPECT_EQ(retrieved->indexCount, 0u);
}
