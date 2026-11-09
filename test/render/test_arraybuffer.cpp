#include "shared/TestVulkanShared.h"

#include "render/buffers/ArrayBuffer.h"

#include <cstring>
#include <stdexcept>
#include <vector>

using namespace neurus;

// ===========================================================================
// Simple test struct (POD, trivially copyable, 24 bytes)
// ===========================================================================

struct alignas(8) TestElement
{
	float x, y, z;   // 12 bytes
	int32_t id;       // 4 bytes
	float value;      // 4 bytes
	float _pad;       // 4 bytes → 24 bytes total
};
static_assert(sizeof(TestElement) == 24, "TestElement must be 24 bytes");

inline bool operator==(const TestElement& a, const TestElement& b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z &&
	       a.id == b.id && a.value == b.value;
}

// ===========================================================================
// GPU test fixture
// ===========================================================================

class ArrayBufferGpuTest : public VulkanTestShared
{
protected:
	/**
	 * @brief Reads back elements from a device-local GPUBuffer into a host vector.
	 *
	 * Creates a host-visible staging buffer, copies device→staging via
	 * vkCmdCopyBuffer, then memcpys to the returned vector.
	 */
	template<typename T>
	std::vector<T> Readback(const GPUBuffer& gpuBuf, uint32_t elementCount)
	{
		const vk::DeviceSize byteSize = static_cast<vk::DeviceSize>(elementCount) * sizeof(T);

		// --- Create host-visible staging buffer ---
		vk::BufferCreateInfo stageCI({}, byteSize, vk::BufferUsageFlagBits::eTransferDst);
		vk::raii::Buffer stageBuf(*m_device, stageCI);

		auto memReqs = stageBuf.getMemoryRequirements();
		uint32_t memTypeIdx = FindMemoryType(PhysicalDevice(), memReqs.memoryTypeBits,
		                                     vk::MemoryPropertyFlagBits::eHostVisible |
		                                     vk::MemoryPropertyFlagBits::eHostCoherent);
		vk::MemoryAllocateInfo allocInfo(memReqs.size, memTypeIdx);
		vk::raii::DeviceMemory stageMem(*m_device, allocInfo);
		stageBuf.bindMemory(*stageMem, 0);

		// --- Copy device-local → staging ---
		{
			auto& cmd = BeginCmd();
			vk::BufferCopy copyRegion(0, 0, byteSize);
			cmd.copyBuffer(gpuBuf.buffer(), *stageBuf, copyRegion);
			EndSubmitWait(cmd);
		}

		// --- Read back to host ---
		std::vector<T> result(elementCount);
		void* mapped = stageMem.mapMemory(0, byteSize);
		std::memcpy(result.data(), mapped, static_cast<size_t>(byteSize));
		stageMem.unmapMemory();

		return result;
	}
};

// ---------------------------------------------------------------------------
// Construction & initial state
// ---------------------------------------------------------------------------

TEST_F(ArrayBufferGpuTest, DefaultConstruction_ZeroSizeAndCapacity)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer,
	                             "TestArray");

	EXPECT_EQ(buf.size(), 0u);
	EXPECT_EQ(buf.capacity(), 0u);
	EXPECT_TRUE(buf.empty());
	EXPECT_EQ(buf.gpuBuffer(), nullptr);
	EXPECT_EQ(buf.buffer(), vk::Buffer{});
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

TEST_F(ArrayBufferGpuTest, Resize_FirstAlloc_CreatesBuffer)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	buf.Resize(4);

	EXPECT_EQ(buf.size(), 0u);       // size tracks logical count, not capacity
	EXPECT_EQ(buf.capacity(), 4u);
	EXPECT_FALSE(buf.empty());
	EXPECT_NE(buf.gpuBuffer(), nullptr);
	EXPECT_NE(buf.buffer(), vk::Buffer{});
}

TEST_F(ArrayBufferGpuTest, Resize_Grow_PreservesData)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	// Upload 2 elements, then grow to 4
	std::vector<TestElement> initial = {
		{  1.0f,  2.0f,  3.0f, 10, 0.5f, 0.0f },
		{ -1.0f, -2.0f, -3.0f, 20, 0.8f, 0.0f },
	};
	buf.Upload(initial);
	EXPECT_EQ(buf.size(), 2u);
	EXPECT_EQ(buf.capacity(), 2u);

	buf.Resize(4);
	EXPECT_EQ(buf.size(), 2u);       // size preserved
	EXPECT_EQ(buf.capacity(), 4u);   // capacity grown

	// Read back and verify first 2 elements
	auto result = Readback<TestElement>(*buf.gpuBuffer(), 4);
	EXPECT_EQ(result[0], initial[0]);
	EXPECT_EQ(result[1], initial[1]);
}

TEST_F(ArrayBufferGpuTest, Resize_Shrink_TruncatesSize)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	std::vector<TestElement> initial = {
		{ 1.0f, 0.0f, 0.0f, 1, 0.1f, 0.0f },
		{ 2.0f, 0.0f, 0.0f, 2, 0.2f, 0.0f },
		{ 3.0f, 0.0f, 0.0f, 3, 0.3f, 0.0f },
		{ 4.0f, 0.0f, 0.0f, 4, 0.4f, 0.0f },
	};
	buf.Upload(initial);
	EXPECT_EQ(buf.size(), 4u);

	buf.Resize(2);
	EXPECT_EQ(buf.size(), 2u);       // truncated
	EXPECT_EQ(buf.capacity(), 2u);

	auto result = Readback<TestElement>(*buf.gpuBuffer(), 2);
	EXPECT_EQ(result[0], initial[0]);
	EXPECT_EQ(result[1], initial[1]);
}

TEST_F(ArrayBufferGpuTest, Resize_ToZero_ReleasesBuffer)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	buf.Resize(8);
	EXPECT_NE(buf.gpuBuffer(), nullptr);

	buf.Resize(0);
	EXPECT_EQ(buf.size(), 0u);
	EXPECT_EQ(buf.capacity(), 0u);
	EXPECT_TRUE(buf.empty());
	EXPECT_EQ(buf.gpuBuffer(), nullptr);
}

TEST_F(ArrayBufferGpuTest, Resize_SameCapacity_IsNoop)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	buf.Resize(4);
	auto* ptrBefore = buf.gpuBuffer();
	EXPECT_EQ(buf.capacity(), 4u);

	buf.Resize(4);
	auto* ptrAfter = buf.gpuBuffer();

	EXPECT_EQ(buf.capacity(), 4u);
	EXPECT_EQ(ptrBefore, ptrAfter);  // same underlying pointer — no reallocation
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

TEST_F(ArrayBufferGpuTest, Upload_Initial_PopulatesData)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	std::vector<TestElement> data = {
		{  5.0f,  6.0f,  7.0f, 100, 0.99f, 0.0f },
		{ -5.0f, -6.0f, -7.0f, 200, 0.11f, 0.0f },
	};
	buf.Upload(data);

	EXPECT_EQ(buf.size(), 2u);
	EXPECT_EQ(buf.capacity(), 2u);

	auto result = Readback<TestElement>(*buf.gpuBuffer(), 2);
	EXPECT_EQ(result[0], data[0]);
	EXPECT_EQ(result[1], data[1]);
}

TEST_F(ArrayBufferGpuTest, Upload_ResizesAutomatically_WhenCapacityTooSmall)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	// Start with a small buffer
	buf.Resize(1);
	EXPECT_EQ(buf.capacity(), 1u);

	std::vector<TestElement> data = {
		{ 10.0f, 20.0f, 30.0f, 1, 0.1f, 0.0f },
		{ 40.0f, 50.0f, 60.0f, 2, 0.2f, 0.0f },
		{ 70.0f, 80.0f, 90.0f, 3, 0.3f, 0.0f },
	};
	buf.Upload(data);

	EXPECT_EQ(buf.size(), 3u);
	EXPECT_GE(buf.capacity(), 3u);

	auto result = Readback<TestElement>(*buf.gpuBuffer(), 3);
	EXPECT_EQ(result[0], data[0]);
	EXPECT_EQ(result[1], data[1]);
	EXPECT_EQ(result[2], data[2]);
}

TEST_F(ArrayBufferGpuTest, Upload_EmptyVector_SetsSizeToZero)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	buf.Resize(4);
	EXPECT_EQ(buf.capacity(), 4u);

	std::vector<TestElement> empty;
	buf.Upload(empty);

	EXPECT_EQ(buf.size(), 0u);
	// Capacity doesn't shrink on empty upload — only Resize(0) releases
}

// ---------------------------------------------------------------------------
// Update (single element)
// ---------------------------------------------------------------------------

TEST_F(ArrayBufferGpuTest, Update_SingleElement_ModifiesCorrectSlot)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	std::vector<TestElement> data = {
		{ 1.0f, 1.0f, 1.0f, 10, 0.5f, 0.0f },
		{ 2.0f, 2.0f, 2.0f, 20, 0.5f, 0.0f },
		{ 3.0f, 3.0f, 3.0f, 30, 0.5f, 0.0f },
	};
	buf.Upload(data);

	// Update element at index 1
	TestElement replacement = { 99.0f, 99.0f, 99.0f, 999, 0.99f, 0.0f };
	buf.Update(replacement, 1);

	auto result = Readback<TestElement>(*buf.gpuBuffer(), 3);
	EXPECT_EQ(result[0], data[0]);        // unchanged
	EXPECT_EQ(result[1], replacement);    // updated
	EXPECT_EQ(result[2], data[2]);        // unchanged
}

TEST_F(ArrayBufferGpuTest, Update_AtZero_ModifiesFirstElement)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	std::vector<TestElement> data = {
		{ 1.0f, 0.0f, 0.0f, 1, 0.1f, 0.0f },
		{ 2.0f, 0.0f, 0.0f, 2, 0.2f, 0.0f },
	};
	buf.Upload(data);

	TestElement replacement = { -1.0f, -1.0f, -1.0f, -1, -0.1f, 0.0f };
	buf.Update(replacement, 0);

	auto result = Readback<TestElement>(*buf.gpuBuffer(), 2);
	EXPECT_EQ(result[0], replacement);
	EXPECT_EQ(result[1], data[1]);
}

TEST_F(ArrayBufferGpuTest, Update_AtLastIndex_ModifiesLastElement)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	std::vector<TestElement> data = {
		{ 1.0f, 0.0f, 0.0f, 1, 0.1f, 0.0f },
		{ 2.0f, 0.0f, 0.0f, 2, 0.2f, 0.0f },
	};
	buf.Upload(data);

	TestElement replacement = { 99.0f, 88.0f, 77.0f, 999, 0.99f, 0.0f };
	buf.Update(replacement, 1);

	auto result = Readback<TestElement>(*buf.gpuBuffer(), 2);
	EXPECT_EQ(result[0], data[0]);
	EXPECT_EQ(result[1], replacement);
}

// ---------------------------------------------------------------------------
// Update — error cases (CPU-only)
// ---------------------------------------------------------------------------

TEST_F(ArrayBufferGpuTest, Update_BeforeAnyAllocation_Throws)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	TestElement elem{};
	EXPECT_THROW(buf.Update(elem, 0), std::out_of_range);
}

TEST_F(ArrayBufferGpuTest, Update_OutOfBounds_Throws)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	std::vector<TestElement> data = { { 1.0f, 0.0f, 0.0f, 1, 0.1f, 0.0f } };
	buf.Upload(data);
	EXPECT_EQ(buf.size(), 1u);

	TestElement elem{};
	EXPECT_THROW(buf.Update(elem, 1), std::out_of_range);  // index == size()
	EXPECT_THROW(buf.Update(elem, 100), std::out_of_range);
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

TEST_F(ArrayBufferGpuTest, MoveConstructor_TransfersOwnership)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> src(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	std::vector<TestElement> data = {
		{ 1.0f, 2.0f, 3.0f, 42, 0.42f, 0.0f },
	};
	src.Upload(data);
	EXPECT_EQ(src.size(), 1u);
	EXPECT_NE(src.gpuBuffer(), nullptr);

	ArrayBuffer<TestElement> dst(std::move(src));

	// dst owns the buffer
	EXPECT_EQ(dst.size(), 1u);
	EXPECT_NE(dst.gpuBuffer(), nullptr);

	// src is in moved-from state (unique_ptr == nullptr)
	EXPECT_EQ(src.gpuBuffer(), nullptr);
	EXPECT_TRUE(src.empty());

	// dst data is intact
	auto result = Readback<TestElement>(*dst.gpuBuffer(), 1);
	EXPECT_EQ(result[0], data[0]);
}

// ---------------------------------------------------------------------------
// Resize after Update — verify data survives
// ---------------------------------------------------------------------------

TEST_F(ArrayBufferGpuTest, ResizeAfterUpdate_PreservesUpdatedData)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ArrayBuffer<TestElement> buf(*m_device, PhysicalDevice(), *m_queue,
	                             m_graphicsQueueFamily,
	                             vk::BufferUsageFlagBits::eStorageBuffer);

	std::vector<TestElement> data = {
		{ 1.0f, 1.0f, 1.0f, 1, 0.1f, 0.0f },
		{ 2.0f, 2.0f, 2.0f, 2, 0.2f, 0.0f },
	};
	buf.Upload(data);

	TestElement updated = { 42.0f, 42.0f, 42.0f, 42, 0.42f, 0.0f };
	buf.Update(updated, 0);

	// Grow: the updated element should be preserved
	buf.Resize(4);

	auto result = Readback<TestElement>(*buf.gpuBuffer(), 4);
	EXPECT_EQ(result[0], updated);   // updated value preserved through resize
	EXPECT_EQ(result[1], data[1]);   // unchanged element preserved
}
