// Must define platform before including Vulkan headers

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include "shared/TestVulkanShared.hpp"

#include "render/DescriptorManager.hpp"
#include "render/buffers/StagingBuffer.hpp"
#include "app/VulkanContext.hpp"

using namespace neurus;

// ---------------------------------------------------------------------------
// Non-GPU tests - Fluent builder (no Vulkan device needed)
// ---------------------------------------------------------------------------

/**
 * @brief Tests for the DescriptorSetLayoutBuilder fluent API.
 *
 * These tests verify binding accumulation without requiring a Vulkan device.
 */

TEST(DescriptorSetLayoutBuilderTest, AddMultipleBindings_BuildsAll)
{
	auto bindings = BuildLayout()
	                    .AddBinding(0, vk::DescriptorType::eUniformBuffer,
	                                vk::ShaderStageFlagBits::eVertex)
	                    .AddBinding(1, vk::DescriptorType::eCombinedImageSampler,
	                                vk::ShaderStageFlagBits::eFragment, 4)
	                    .AddBinding(2, vk::DescriptorType::eStorageBuffer,
	                                vk::ShaderStageFlagBits::eCompute)
	                    .Build();

	ASSERT_EQ(bindings.size(), 3u);

	EXPECT_EQ(bindings[0].binding, 0u);
	EXPECT_EQ(bindings[0].descriptorType, vk::DescriptorType::eUniformBuffer);

	EXPECT_EQ(bindings[0].descriptorCount, 1u);
	EXPECT_EQ(bindings[0].stageFlags, vk::ShaderStageFlagBits::eVertex);

	EXPECT_EQ(bindings[1].descriptorCount, 4u);
	EXPECT_EQ(bindings[1].binding, 1u);
	EXPECT_EQ(bindings[1].descriptorType, vk::DescriptorType::eCombinedImageSampler);

	EXPECT_EQ(bindings[2].binding, 2u);
	EXPECT_EQ(bindings[2].descriptorType, vk::DescriptorType::eStorageBuffer);
}

TEST(DescriptorSetLayoutBuilderTest, EmptyBuilder_ReturnsEmptyVector)
{
	auto bindings = BuildLayout().Build();
	EXPECT_TRUE(bindings.empty());
}

// ---------------------------------------------------------------------------
// GPU tests - Layout, Pool, Set allocation and writing
// ---------------------------------------------------------------------------

/**
 * @brief GPU test fixture for descriptor management tests.
 *
 * Creates an instance and device. Tests are skipped if no Vulkan-capable
 * GPU is available.
 */
class DescriptorManagerTest : public VulkanTestShared
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

// ---------------------------------------------------------------------------
// DescriptorSetLayout creation
// ---------------------------------------------------------------------------

TEST_F(DescriptorManagerTest, CreateDescriptorSetLayout_WithSingleBinding)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_THROW({
		auto layout = BuildLayout()
		                    .AddBinding(0, vk::DescriptorType::eUniformBuffer,
		                                vk::ShaderStageFlagBits::eVertex)
		                    .Build(*m_device);
		EXPECT_EQ(layout.bindings().size(), 1u);
	});
}

TEST_F(DescriptorManagerTest, CreateDescriptorSetLayout_WithMultipleBindings)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_THROW({
		auto layout = BuildLayout()
		                    .AddBinding(0, vk::DescriptorType::eUniformBuffer,
		                                vk::ShaderStageFlagBits::eVertex)
		                    .AddBinding(1, vk::DescriptorType::eCombinedImageSampler,
		                                vk::ShaderStageFlagBits::eFragment)
		                    .Build(*m_device);
		EXPECT_EQ(layout.bindings().size(), 2u);
	});
}

TEST_F(DescriptorManagerTest, DescriptorSetLayout_ReturnsValidVkLayout)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto layout = BuildLayout()
	                  .AddBinding(0, vk::DescriptorType::eUniformBuffer,
	                              vk::ShaderStageFlagBits::eVertex)
	                  .Build(*m_device);

	// The underlying vk::raii::DescriptorSetLayout must be non-null
	const auto& vkLayout = layout.layout();
	EXPECT_NE(*vkLayout, vk::DescriptorSetLayout{});
}

// ---------------------------------------------------------------------------
// DescriptorPool creation
// ---------------------------------------------------------------------------

TEST_F(DescriptorManagerTest, CreateDescriptorPool_WithPoolSizes)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	std::vector<vk::DescriptorPoolSize> poolSizes = {
		{vk::DescriptorType::eUniformBuffer, 8},
		{vk::DescriptorType::eCombinedImageSampler, 4},
	};

	ASSERT_NO_THROW({
		DescriptorPool pool(*m_device, 16, poolSizes);
	});
}

TEST_F(DescriptorManagerTest, CreateDescriptorPool_WithZeroMaxSets)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	std::vector<vk::DescriptorPoolSize> poolSizes = {
		{vk::DescriptorType::eUniformBuffer, 1},
	};

	// Pool with maxSets=0 is valid per spec (can allocate 0 sets)
	ASSERT_NO_THROW({
		DescriptorPool pool(*m_device, 0, poolSizes);
	});
}

// ---------------------------------------------------------------------------
// CalculatePoolSizes
// ---------------------------------------------------------------------------

TEST_F(DescriptorManagerTest, CalculatePoolSizes_SingleLayout)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto layout = BuildLayout()
	                 .AddBinding(0, vk::DescriptorType::eUniformBuffer,
	                             vk::ShaderStageFlagBits::eVertex)
	                 .AddBinding(1, vk::DescriptorType::eCombinedImageSampler,
	                             vk::ShaderStageFlagBits::eFragment)
	                 .Build(*m_device);
	std::vector<const DescriptorSetLayout*> layouts = {&layout};

	auto sizes = DescriptorPool::CalculatePoolSizes(layouts, 2);

	// We expect: UBO: 2 (1 × 2), CombinedImageSampler: 2 (1 × 2)
	bool foundUbo = false;
	bool foundSampler = false;
	for (const auto& s : sizes)
	{
		if (s.type == vk::DescriptorType::eUniformBuffer)
		{
			EXPECT_GE(s.descriptorCount, 2u);
			foundUbo = true;
		}
		if (s.type == vk::DescriptorType::eCombinedImageSampler)
		{
			EXPECT_GE(s.descriptorCount, 2u);
			foundSampler = true;
		}
	}
	EXPECT_TRUE(foundUbo);
	EXPECT_TRUE(foundSampler);
}

TEST_F(DescriptorManagerTest, CalculatePoolSizes_MultipleLayouts)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto layoutA = BuildLayout()
	                  .AddBinding(0,
	                              vk::DescriptorType::eUniformBuffer,
	                              vk::ShaderStageFlagBits::eVertex)
	                  .Build(*m_device);

	auto layoutB = BuildLayout()
	                  .AddBinding(0,
	                              vk::DescriptorType::eUniformBuffer,
	                              vk::ShaderStageFlagBits::eFragment)
	                  .AddBinding(1,
	                              vk::DescriptorType::eStorageBuffer,
	                              vk::ShaderStageFlagBits::eFragment)
	                  .Build(*m_device);
	std::vector<const DescriptorSetLayout*> layouts = {&layoutA, &layoutB};

	auto sizes = DescriptorPool::CalculatePoolSizes(layouts, 3);

	// UBO appears in both layouts: (1+1)×3 = 6
	bool foundUbo = false;
	for (const auto& s : sizes)
	{
		if (s.type == vk::DescriptorType::eUniformBuffer)
		{
			EXPECT_GE(s.descriptorCount, 6u);
			foundUbo = true;
		}
	}
	EXPECT_TRUE(foundUbo);
}

// ---------------------------------------------------------------------------
// DescriptorSet allocation
// ---------------------------------------------------------------------------

TEST_F(DescriptorManagerTest, Allocate_SingleDescriptorSet_Succeeds)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto layout = BuildLayout()
	                 .AddBinding(0, vk::DescriptorType::eUniformBuffer,
	                             vk::ShaderStageFlagBits::eVertex)
	                 .Build(*m_device);

	std::vector<vk::DescriptorPoolSize> poolSizes = {
		{vk::DescriptorType::eUniformBuffer, 1},
	};
	DescriptorPool pool(*m_device, 1, poolSizes);

	auto sets = pool.Allocate(layout, 1);
	ASSERT_EQ(sets.size(), 1u);

	// Handle must be valid (non-null)
	EXPECT_NE(sets[0].handle(), vk::DescriptorSet{});
}

TEST_F(DescriptorManagerTest, Allocate_MultipleDescriptorSets_Succeeds)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto layout = BuildLayout()
	                 .AddBinding(0, vk::DescriptorType::eUniformBuffer,
	                             vk::ShaderStageFlagBits::eVertex)
	                 .Build(*m_device);

	std::vector<vk::DescriptorPoolSize> poolSizes = {
		{vk::DescriptorType::eUniformBuffer, 3},
	};
	DescriptorPool pool(*m_device, 3, poolSizes);

	auto sets = pool.Allocate(layout, 3);
	ASSERT_EQ(sets.size(), 3u);

	// All handles must be non-null and unique
	EXPECT_NE(sets[0].handle(), vk::DescriptorSet{});
	EXPECT_NE(sets[1].handle(), vk::DescriptorSet{});
	EXPECT_NE(sets[2].handle(), vk::DescriptorSet{});
	EXPECT_NE(sets[0].handle(), sets[1].handle());
	EXPECT_NE(sets[1].handle(), sets[2].handle());
	EXPECT_NE(sets[0].handle(), sets[2].handle());
}

TEST_F(DescriptorManagerTest, Allocate_ZeroCount_ReturnsEmpty)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto layout = BuildLayout()
	                 .AddBinding(0, vk::DescriptorType::eUniformBuffer,
	                             vk::ShaderStageFlagBits::eVertex)
	                 .Build(*m_device);

	std::vector<vk::DescriptorPoolSize> poolSizes = {
		{vk::DescriptorType::eUniformBuffer, 1},
	};
	DescriptorPool pool(*m_device, 1, poolSizes);

	auto sets = pool.Allocate(layout, 0);
	EXPECT_TRUE(sets.empty());
}

// ---------------------------------------------------------------------------
// DescriptorSet write operations
// ---------------------------------------------------------------------------

TEST_F(DescriptorManagerTest, WriteBuffer_OnAllocatedSet_Succeeds)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// Create a host-visible staging buffer
	constexpr vk::DeviceSize kBufSize = 256;
	StagingBuffer buf(*m_device,
	                  PhysicalDevice(),
	                  kBufSize);

	vk::DescriptorBufferInfo bufInfo = buf.GetDescriptorInfo();

	// Create layout, pool, allocate set
	auto layout = BuildLayout()
	                 .AddBinding(0, vk::DescriptorType::eUniformBuffer,
	                             vk::ShaderStageFlagBits::eVertex)
	                 .Build(*m_device);

	std::vector<vk::DescriptorPoolSize> poolSizes = {
		{vk::DescriptorType::eUniformBuffer, 1},
	};
	DescriptorPool pool(*m_device, 1, poolSizes);

	auto sets = pool.Allocate(layout, 1);
	ASSERT_EQ(sets.size(), 1u);

	// Write the buffer descriptor - must not throw
	ASSERT_NO_THROW({
		sets[0].WriteBuffer(0, bufInfo,
		                    vk::DescriptorType::eUniformBuffer);
	});
}

TEST_F(DescriptorManagerTest, WriteBuffer_DefaultType_UniformBuffer)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	constexpr vk::DeviceSize kBufSize = 128;
	StagingBuffer buf(*m_device,
	                  PhysicalDevice(),
	                  kBufSize);

	vk::DescriptorBufferInfo bufInfo = buf.GetDescriptorInfo();

	auto layout = BuildLayout()
	                 .AddBinding(0, vk::DescriptorType::eStorageBuffer,
	                             vk::ShaderStageFlagBits::eCompute)
	                 .Build(*m_device);

	std::vector<vk::DescriptorPoolSize> poolSizes = {
		{vk::DescriptorType::eStorageBuffer, 1},
	};
	DescriptorPool pool(*m_device, 1, poolSizes);

	auto sets = pool.Allocate(layout, 1);
	ASSERT_EQ(sets.size(), 1u);

	// Default type should be UniformBuffer; we override to StorageBuffer
	ASSERT_NO_THROW({
		sets[0].WriteBuffer(0, bufInfo,
		                    vk::DescriptorType::eStorageBuffer);
	});
}

// ---------------------------------------------------------------------------
// Standard layout patterns (demonstrated through tests per spec)
// ---------------------------------------------------------------------------

TEST_F(DescriptorManagerTest, StandardLayout_CameraUBO)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// Camera UBO: set 0, binding 0 - uniform buffer, vertex stage
	auto bindings = BuildLayout()
	                    .AddBinding(0, vk::DescriptorType::eUniformBuffer,
	                                vk::ShaderStageFlagBits::eVertex)
	                    .Build();

	ASSERT_EQ(bindings.size(), 1u);
	EXPECT_EQ(bindings[0].binding, 0u);
	EXPECT_EQ(bindings[0].descriptorType, vk::DescriptorType::eUniformBuffer);

	DescriptorSetLayout layout(*m_device, bindings);
	EXPECT_EQ(layout.bindings().size(), 1u);
}

TEST_F(DescriptorManagerTest, StandardLayout_MaterialUBOAndTexture)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// Material UBO (set 1, binding 0) + texture sampler (set 1, binding 1+)
	auto bindings = BuildLayout()
	                    .AddBinding(0, vk::DescriptorType::eUniformBuffer,
	                                vk::ShaderStageFlagBits::eFragment)
	                    .AddBinding(1,
	                                vk::DescriptorType::eCombinedImageSampler,
	                                vk::ShaderStageFlagBits::eFragment)
	                    .AddBinding(2,
	                                vk::DescriptorType::eCombinedImageSampler,
	                                vk::ShaderStageFlagBits::eFragment)
	                    .Build();

	ASSERT_EQ(bindings.size(), 3u);
	EXPECT_EQ(bindings[0].descriptorType, vk::DescriptorType::eUniformBuffer);
	EXPECT_EQ(bindings[1].descriptorType,
	          vk::DescriptorType::eCombinedImageSampler);
	EXPECT_EQ(bindings[2].descriptorType,
	          vk::DescriptorType::eCombinedImageSampler);

	DescriptorSetLayout layout(*m_device, bindings);
	EXPECT_EQ(layout.bindings().size(), 3u);
}
