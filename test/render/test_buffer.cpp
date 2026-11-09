#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/buffers/StagingBuffer.h"
#include "render/buffers/GPUBuffer.h"
#include "render/buffers/UniformBuffer.h"

#include <array>
#include <cstring>

using namespace neurus;

/**
 * @brief Test struct for UniformBuffer - 16 bytes (4 floats).
 */
struct TestUBOData
{
	float a, b, c, d;
};

/**
 * @brief Tests for Buffer class hierarchy: StagingBuffer, GPUBuffer, UniformBuffer.
 *
 * Covers construction, Map/Unmap, Upload, GetDescriptorInfo, and move semantics.
 *
 * @note Requires a Vulkan 1.4-capable GPU. Skipped in CI without GPU.
 */
class BufferTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
	}

	void TearDown() override
	{
		VulkanTestShared::TearDown();
	}
};

// ===========================================================================
// StagingBuffer tests
// ===========================================================================

TEST_F(BufferTest, StagingBuffer_Create_Succeeds)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	constexpr vk::DeviceSize kSize = 256;

	ASSERT_NO_THROW({
		StagingBuffer buf(*m_device,
		                  PhysicalDevice(),
		                  kSize);
		EXPECT_EQ(buf.size(), kSize);
		EXPECT_NE(buf.buffer(), vk::Buffer{});
	});
}

TEST_F(BufferTest, StagingBuffer_MapWriteThenRead)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	constexpr vk::DeviceSize kSize = 32;
	std::array<uint8_t, kSize> data;
	for (size_t i = 0; i < kSize; ++i)
	{
		data[i] = static_cast<uint8_t>(~i);
	}

	StagingBuffer buf(*m_device,
	                  PhysicalDevice(),
	                  kSize);

	// Direct Map + write
	void* mapped = buf.Map();
	ASSERT_NE(mapped, nullptr);
	std::memcpy(mapped, data.data(), kSize);
	buf.Unmap();

	// Map again + read-back
	mapped = buf.Map();
	ASSERT_NE(mapped, nullptr);
	for (size_t i = 0; i < kSize; ++i)
	{
		EXPECT_EQ(static_cast<const uint8_t*>(mapped)[i], data[i])
			<< "Byte mismatch at offset " << i;
	}
	buf.Unmap();
}

TEST_F(BufferTest, StagingBuffer_Upload_DataRoundtrips)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	constexpr vk::DeviceSize kSize = 64;
	std::array<uint8_t, kSize> srcData;
	for (size_t i = 0; i < kSize; ++i)
	{
		srcData[i] = static_cast<uint8_t>(i * 7);
	}

	StagingBuffer buf(*m_device,
	                  PhysicalDevice(),
	                  kSize);

	// Upload via Upload (Map + memcpy + Unmap internally)
	ASSERT_NO_THROW(buf.Upload(srcData.data(), kSize));

	// Read back via Map
	void* mapped = buf.Map();
	ASSERT_NE(mapped, nullptr);

	int cmp = std::memcmp(mapped, srcData.data(), kSize);
	EXPECT_EQ(cmp, 0) << "Data mismatch after upload + map read-back";

	buf.Unmap();
}

// ===========================================================================
// GPUBuffer tests
// ===========================================================================

TEST_F(BufferTest, GPUBuffer_CreateDeviceLocal_Succeeds)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	constexpr vk::DeviceSize kSize = 1024;

	ASSERT_NO_THROW({
		GPUBuffer buf(*m_device,
		              PhysicalDevice(),
		              *m_queue,
		              m_graphicsQueueFamily,
		              kSize,
		              vk::BufferUsageFlagBits::eVertexBuffer);
		EXPECT_EQ(buf.size(), kSize);
		EXPECT_NE(buf.buffer(), vk::Buffer{});
	});
}

TEST_F(BufferTest, GPUBuffer_Upload_DataRoundtrips)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	constexpr vk::DeviceSize kSize = 64;
	std::array<uint8_t, kSize> srcData;
	for (size_t i = 0; i < kSize; ++i)
	{
		srcData[i] = static_cast<uint8_t>(i * 7);
	}

	GPUBuffer buf(*m_device,
	              PhysicalDevice(),
	              *m_queue,
	              m_graphicsQueueFamily,
	              kSize,
	              vk::BufferUsageFlagBits::eStorageBuffer);

	// Upload via staging-backed Map/Unmap
	ASSERT_NO_THROW(buf.Upload(srcData.data(), kSize));

	// After Upload, the staging buffer still holds the source data.
	// Map() returns the staging pointer, so we can verify the upload data
	// passed through staging correctly.
	void* mapped = buf.Map();
	ASSERT_NE(mapped, nullptr);

	int cmp = std::memcmp(mapped, srcData.data(), kSize);
	EXPECT_EQ(cmp, 0) << "Data mismatch after staging-backed upload";

	buf.Unmap();
}

// ===========================================================================
// UniformBuffer tests
// ===========================================================================

TEST_F(BufferTest, UniformBuffer_Create_Succeeds)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_THROW({
		UniformBuffer<TestUBOData> ubo(*m_device,
		                               PhysicalDevice());
		EXPECT_EQ(ubo.size(), sizeof(TestUBOData));
		EXPECT_NE(ubo.buffer(), vk::Buffer{});
	});
}

TEST_F(BufferTest, UniformBuffer_Upload_DataRoundtrips)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	TestUBOData data = { 1.0f, 2.0f, 3.0f, 4.0f };

	UniformBuffer<TestUBOData> ubo(*m_device,
	                               PhysicalDevice());

	// Upload the struct via type-safe Upload
	ASSERT_NO_THROW(ubo.Upload(data));

	// Read back via Map
	void* mapped = ubo.Map();
	ASSERT_NE(mapped, nullptr);

	const auto* readback = static_cast<const TestUBOData*>(mapped);
	EXPECT_EQ(readback->a, data.a);
	EXPECT_EQ(readback->b, data.b);
	EXPECT_EQ(readback->c, data.c);
	EXPECT_EQ(readback->d, data.d);

	ubo.Unmap();
}

// ===========================================================================
// GetDescriptorInfo tests
// ===========================================================================

TEST_F(BufferTest, GetDescriptorInfo_ReturnsValidInfo)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// --- Test on GPUBuffer ---
	{
		constexpr vk::DeviceSize kSize = 128;

		GPUBuffer buf(*m_device,
		              PhysicalDevice(),
		              *m_queue,
		              m_graphicsQueueFamily,
		              kSize,
		              vk::BufferUsageFlagBits::eUniformBuffer);

		vk::DescriptorBufferInfo info = buf.GetDescriptorInfo();

		EXPECT_NE(info.buffer, vk::Buffer{});
		EXPECT_EQ(info.offset, 0u);
		EXPECT_EQ(info.range, kSize);

		// With explicit offset and range
		vk::DescriptorBufferInfo partialInfo = buf.GetDescriptorInfo(16, 32);
		EXPECT_EQ(partialInfo.offset, 16u);
		EXPECT_EQ(partialInfo.range, 32u);
	}

	// --- Test on UniformBuffer ---
	{
		UniformBuffer<TestUBOData> ubo(*m_device,
		                               PhysicalDevice());

		vk::DescriptorBufferInfo info = ubo.GetDescriptorInfo();

		EXPECT_NE(info.buffer, vk::Buffer{});
		EXPECT_EQ(info.offset, 0u);
		EXPECT_EQ(info.range, sizeof(TestUBOData));
	}
}

// ===========================================================================
// Move semantics tests
// ===========================================================================

TEST_F(BufferTest, MoveConstructor_TransfersOwnership)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	constexpr vk::DeviceSize kSize = 64;

	GPUBuffer bufA(*m_device,
	               PhysicalDevice(),
	               *m_queue,
	               m_graphicsQueueFamily,
	               kSize,
	               vk::BufferUsageFlagBits::eUniformBuffer);

	vk::Buffer origHandle = bufA.buffer();

	// Move-construct
	GPUBuffer bufB(std::move(bufA));

	// bufB should now own the resource
	EXPECT_EQ(bufB.size(), kSize);
	EXPECT_EQ(bufB.buffer(), origHandle);
}

TEST_F(BufferTest, MoveAssignment_TransfersOwnership)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	constexpr vk::DeviceSize kSize = 64;

	GPUBuffer bufA(*m_device,
	               PhysicalDevice(),
	               *m_queue,
	               m_graphicsQueueFamily,
	               kSize,
	               vk::BufferUsageFlagBits::eUniformBuffer);

	vk::Buffer origHandle = bufA.buffer();

	// Move-assign
	GPUBuffer bufB(*m_device,
	               PhysicalDevice(),
	               *m_queue,
	               m_graphicsQueueFamily,
	               32,  // different size
	               vk::BufferUsageFlagBits::eStorageBuffer);

	bufB = std::move(bufA);

	EXPECT_EQ(bufB.size(), kSize);
	EXPECT_EQ(bufB.buffer(), origHandle);
}

// ===========================================================================
// buffer() accessor test
// ===========================================================================

TEST_F(BufferTest, BufferAccessor_ReturnsValidHandle)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	constexpr vk::DeviceSize kSize = 16;

	GPUBuffer buf(*m_device,
	              PhysicalDevice(),
	              *m_queue,
	              m_graphicsQueueFamily,
	              kSize,
	              vk::BufferUsageFlagBits::eUniformBuffer);

	EXPECT_NE(buf.buffer(), vk::Buffer{});
	EXPECT_EQ(buf.size(), kSize);
}
