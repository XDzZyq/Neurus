#include "shared/TestVulkanShared.h"

#include <array>
#include <cstring>
#include <vector>

#include "render/buffers/BufferLayout.h"
#include "render/buffers/VertexBuffer.h"
#include "render/buffers/IndexBuffer.h"

using namespace neurus;

// ===========================================================================
// BufferLayout - pure CPU tests (no Vulkan required)
// ===========================================================================

TEST(BufferLayoutTest, EmptyLayout_HasZeroAttributes)
{
	BufferLayout layout;
	EXPECT_EQ(layout.GetAttributeCount(), 0u);
	EXPECT_EQ(layout.GetStride(), 0u);
}

TEST(BufferLayoutTest, AddSingleAttribute_IncrementsCount)
{
	BufferLayout layout;
	layout.AddAttribute(0, vk::Format::eR32G32B32Sfloat, 0);

	EXPECT_EQ(layout.GetAttributeCount(), 1u);
	EXPECT_EQ(layout.GetStride(), 12u);  // vec3 = 12 bytes
}

TEST(BufferLayoutTest, AddMultipleAttributes_ComputesCorrectStride)
{
	// Typical vertex layout: position(12) + normal(12) + uv(8)
	BufferLayout layout;
	layout.AddAttribute(0, vk::Format::eR32G32B32Sfloat, 0);   // position: 0..11
	layout.AddAttribute(1, vk::Format::eR32G32B32Sfloat, 12);  // normal:   12..23
	layout.AddAttribute(2, vk::Format::eR32G32Sfloat, 24);     // uv:       24..31

	EXPECT_EQ(layout.GetAttributeCount(), 3u);
	EXPECT_EQ(layout.GetStride(), 32u);  // 24 + 8 = 32
}

TEST(BufferLayoutTest, GetBindingDescription_ReturnsCorrectValues)
{
	BufferLayout layout;
	layout.AddAttribute(0, vk::Format::eR32G32B32Sfloat, 0);
	layout.AddAttribute(1, vk::Format::eR32G32Sfloat, 12);

	auto binding = layout.GetBindingDescription();

	EXPECT_EQ(binding.binding, 0u);
	EXPECT_EQ(binding.stride, 20u);  // 12 + 8 = 20
	EXPECT_EQ(binding.inputRate, vk::VertexInputRate::eVertex);
}

TEST(BufferLayoutTest, GetAttributeDescriptions_ReturnsCorrectAttributes)
{
	BufferLayout layout;
	layout.AddAttribute(0, vk::Format::eR32G32B32Sfloat, 0);
	layout.AddAttribute(1, vk::Format::eR32G32Sfloat, 12);
	layout.AddAttribute(2, vk::Format::eR32G32B32A32Sfloat, 20);

	const auto& attrs = layout.GetAttributeDescriptions();
	ASSERT_EQ(attrs.size(), 3u);

	// Attribute 0: position
	EXPECT_EQ(attrs[0].location, 0u);
	EXPECT_EQ(attrs[0].binding, 0u);
	EXPECT_EQ(attrs[0].format, vk::Format::eR32G32B32Sfloat);
	EXPECT_EQ(attrs[0].offset, 0u);

	// Attribute 1: uv
	EXPECT_EQ(attrs[1].location, 1u);
	EXPECT_EQ(attrs[1].format, vk::Format::eR32G32Sfloat);
	EXPECT_EQ(attrs[1].offset, 12u);

	// Attribute 2: color
	EXPECT_EQ(attrs[2].location, 2u);
	EXPECT_EQ(attrs[2].format, vk::Format::eR32G32B32A32Sfloat);
	EXPECT_EQ(attrs[2].offset, 20u);
}

TEST(BufferLayoutTest, GetFormatSize_CommonFormats)
{
	EXPECT_EQ(BufferLayout::GetFormatSize(vk::Format::eR32Sfloat), 4u);
	EXPECT_EQ(BufferLayout::GetFormatSize(vk::Format::eR32G32Sfloat), 8u);
	EXPECT_EQ(BufferLayout::GetFormatSize(vk::Format::eR32G32B32Sfloat), 12u);
	EXPECT_EQ(BufferLayout::GetFormatSize(vk::Format::eR32G32B32A32Sfloat), 16u);
	EXPECT_EQ(BufferLayout::GetFormatSize(vk::Format::eR8G8B8A8Unorm), 4u);
	EXPECT_EQ(BufferLayout::GetFormatSize(vk::Format::eR16G16B16A16Sfloat), 8u);
	EXPECT_EQ(BufferLayout::GetFormatSize(vk::Format::eR16Sfloat), 2u);
	EXPECT_EQ(BufferLayout::GetFormatSize(vk::Format::eR16G16Sfloat), 4u);
}

// ===========================================================================
// VertexBuffer + IndexBuffer - GPU tests
// ===========================================================================

/**
 * @brief GPU test fixture for buffer operations (VBO/IBO).
 *
 * Inherits Vulkan bootstrap (instance, device, queue) from VulkanTestShared.
 */
class BufferGpuTest : public VulkanTestShared
{
protected:
};

// ---------------------------------------------------------------------------
// VertexBuffer
// ---------------------------------------------------------------------------

TEST_F(BufferGpuTest, VertexBuffer_CreateAndUpload_StoresCorrectMetadata)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// 3 vertices of { float x, y, z } = 3 * 12 = 36 bytes
	constexpr uint32_t kVertexCount = 3;
	constexpr uint32_t kStride = 12;  // 3 floats
	constexpr vk::DeviceSize kSize = kVertexCount * kStride;

	std::array<float, kVertexCount * 3> vertexData = {
		0.0f, 0.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f
	};

	VertexBuffer vbo(*m_device,
	                 PhysicalDevice(),
	                 *m_queue,
	                 m_graphicsQueueFamily,
	                 vertexData.data(),
	                 kSize,
	                 kStride,
	                 kVertexCount);

	EXPECT_EQ(vbo.GetVertexCount(), kVertexCount);
	EXPECT_EQ(vbo.GetStride(), kStride);
	EXPECT_NE(vbo.buffer(), vk::Buffer{});
}

// ---------------------------------------------------------------------------
// IndexBuffer
// ---------------------------------------------------------------------------

TEST_F(BufferGpuTest, IndexBuffer_CreateAndUpload_StoresCorrectMetadata)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// 6 indices for a quad (2 triangles)
	constexpr uint32_t kIndexCount = 6;
	constexpr vk::DeviceSize kSize = kIndexCount * sizeof(uint32_t);

	std::array<uint32_t, kIndexCount> indexData = { 0, 1, 2, 0, 2, 3 };

	IndexBuffer ibo(*m_device,
	                PhysicalDevice(),
	                *m_queue,
	                m_graphicsQueueFamily,
	                indexData.data(),
	                kSize,
	                kIndexCount);

	EXPECT_EQ(ibo.GetIndexCount(), kIndexCount);
	EXPECT_EQ(ibo.GetIndexType(), vk::IndexType::eUint32);
	EXPECT_NE(ibo.buffer(), vk::Buffer{});
}

// ---------------------------------------------------------------------------
// VertexBuffer + IndexBuffer combined (typical usage)
// ---------------------------------------------------------------------------

TEST_F(BufferGpuTest, VertexAndIndexBuffer_TypicalMesh)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// A simple quad: 4 vertices, 6 indices
	constexpr uint32_t kVertexCount = 4;
	constexpr uint32_t kStride = 20;  // pos(12) + uv(8)
	constexpr uint32_t kIndexCount = 6;

	// Interleaved vertex data: position (3 floats) + uv (2 floats)
	std::array<float, kVertexCount * 5> vertexData = {
		// pos x,y,z   uv u,v
		-0.5f, -0.5f, 0.0f,  0.0f, 0.0f,  // vertex 0
		 0.5f, -0.5f, 0.0f,  1.0f, 0.0f,  // vertex 1
		 0.5f,  0.5f, 0.0f,  1.0f, 1.0f,  // vertex 2
		-0.5f,  0.5f, 0.0f,  0.0f, 1.0f   // vertex 3
	};

	std::array<uint32_t, kIndexCount> indexData = { 0, 1, 2, 0, 2, 3 };

	vk::DeviceSize vertexDataSize = kVertexCount * kStride;
	vk::DeviceSize indexDataSize = kIndexCount * sizeof(uint32_t);

	VertexBuffer vbo(*m_device,
	                 PhysicalDevice(),
	                 *m_queue,
	                 m_graphicsQueueFamily,
	                 vertexData.data(),
	                 vertexDataSize,
	                 kStride,
	                 kVertexCount);

	IndexBuffer ibo(*m_device,
	                PhysicalDevice(),
	                *m_queue,
	                m_graphicsQueueFamily,
	                indexData.data(),
	                indexDataSize,
	                kIndexCount);

	EXPECT_EQ(vbo.GetVertexCount(), kVertexCount);
	EXPECT_EQ(vbo.GetStride(), kStride);
	EXPECT_EQ(ibo.GetIndexCount(), kIndexCount);

	// Buffers must have distinct handles
	EXPECT_NE(vbo.buffer(), ibo.buffer());
}
