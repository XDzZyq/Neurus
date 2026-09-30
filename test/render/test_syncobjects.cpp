// Must define platform before including Vulkan headers


#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"
#include "render/passes/SyncObjects.h"
#include "render/Barrier.h"
#include "render/Image.h"

#include <chrono>
#include <memory>
#include <vector>

using namespace neurus;

// ============================================================================
// Device-dependent tests - require a Vulkan-capable GPU
// ============================================================================

class SyncObjectsDeviceTest : public VulkanTestShared
{
protected:
	// VulkanTestShared provides SetUp/TearDown with full Vulkan bootstrap,
	// plus m_hasVulkan, m_device, PhysicalDevice(), and graphics queue helpers.
};

// ============================================================================
// Barrier::ToVulkanImageState tests
// ============================================================================

TEST_F(SyncObjectsDeviceTest, Barrier_ToVulkanImageState_Basic)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";
	auto& pd = PhysicalDevice();
	Image image(*m_device, pd, {64, 64}, vk::Format::eR8G8B8A8Unorm,
	            vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, 1);

	auto state = Barrier::ToVulkanImageState(ImageState::Undefined);
	EXPECT_EQ(state.layout, vk::ImageLayout::eUndefined);
	// eAllCommands, not eTopOfPipe: a discard transition still needs an execution
	// dependency against whoever last read the image (see Barrier.cpp).
	EXPECT_EQ(state.stage, vk::PipelineStageFlagBits2::eAllCommands);
	EXPECT_EQ(state.access, vk::AccessFlagBits2::eNone);

	state = Barrier::ToVulkanImageState(ImageState::ColorAttachment);
	EXPECT_EQ(state.layout, vk::ImageLayout::eColorAttachmentOptimal);

	state = Barrier::ToVulkanImageState(ImageState::TransferSrc);
	EXPECT_EQ(state.layout, vk::ImageLayout::eTransferSrcOptimal);

	state = Barrier::ToVulkanImageState(ImageState::TransferDst);
	EXPECT_EQ(state.layout, vk::ImageLayout::eTransferDstOptimal);

	state = Barrier::ToVulkanImageState(ImageState::ColorShaderRead);
	EXPECT_EQ(state.layout, vk::ImageLayout::eShaderReadOnlyOptimal);

	state = Barrier::ToVulkanImageState(ImageState::ShaderWrite);
	EXPECT_EQ(state.layout, vk::ImageLayout::eGeneral);

	state = Barrier::ToVulkanImageState(ImageState::Present);
	EXPECT_EQ(state.layout, vk::ImageLayout::ePresentSrcKHR);
}

TEST_F(SyncObjectsDeviceTest, Barrier_ToVulkanImageState_DepthShaderRead)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";
	auto& pd = PhysicalDevice();
	auto fmtProps = pd.getFormatProperties(vk::Format::eD32Sfloat);
	if (!(fmtProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment))
		GTEST_SKIP() << "D32_SFLOAT depth attachment not supported.";

	Image image(*m_device, pd, {64, 64}, vk::Format::eD32Sfloat,
	            vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
	            1, Image::ImageType::eDepthStencil);

	auto state = Barrier::ToVulkanImageState(ImageState::DepthShaderRead);
	EXPECT_EQ(state.layout, vk::ImageLayout::eDepthStencilReadOnlyOptimal);
}

/**
 * @test Default-constructed Fence (unsignaled) can be created without error.
 */
TEST_F(SyncObjectsDeviceTest, Fence_CreateDefault)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_THROW({
		Fence fence(*m_device);
		EXPECT_TRUE(*fence.handle());
	});
}

/**
 * @test Fence can be created in the signaled state.
 */
TEST_F(SyncObjectsDeviceTest, Fence_CreateSignaled)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_THROW({
		Fence fence(*m_device, vk::FenceCreateFlagBits::eSignaled);
		EXPECT_TRUE(*fence.handle());
	});
}

/**
 * @test WaitAndReset on a signaled fence returns true and resets the fence.
 *
 * After the first successful WaitAndReset the fence is unsignaled, so a second
 * call with zero timeout must return false.
 */
TEST_F(SyncObjectsDeviceTest, Fence_WaitAndReset_Signaled)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	Fence fence(*m_device, vk::FenceCreateFlagBits::eSignaled);

	// First wait - fence is signaled, should succeed immediately
	EXPECT_TRUE(fence.WaitAndReset(0));

	// Second wait - fence was reset to unsignaled, should time out immediately
	EXPECT_FALSE(fence.WaitAndReset(0));
}

/**
 * @test WaitAndReset on an unsignaled fence times out (returns false).
 */
TEST_F(SyncObjectsDeviceTest, Fence_WaitAndReset_Unsignaled)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	Fence fence(*m_device);  // unsignaled by default

	// Zero timeout: fence is not signaled, so wait should fail immediately
	EXPECT_FALSE(fence.WaitAndReset(0));
}

/**
 * @test WaitAndReset on a signaled fence completes in well under 10 ms.
 *
 * Validates the timing constraint: a pre-signaled fence must be observable
 * without blocking the CPU.
 */
TEST_F(SyncObjectsDeviceTest, Fence_WaitAndReset_Timing)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	Fence fence(*m_device, vk::FenceCreateFlagBits::eSignaled);

	auto start = std::chrono::steady_clock::now();
	bool result = fence.WaitAndReset(kDefaultFenceTimeoutNs);
	auto elapsed = std::chrono::steady_clock::now() - start;

	EXPECT_TRUE(result);
	auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
	EXPECT_LT(elapsedMs, 10) << "WaitAndReset on a signaled fence took " << elapsedMs << " ms";
}

/**
 * @test Semaphore can be created without error.
 */
TEST_F(SyncObjectsDeviceTest, Semaphore_Create)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_THROW({
		Semaphore semaphore(*m_device);
		EXPECT_TRUE(*semaphore.handle());
	});
}

/**
 * @test FrameSync constructs all three synchronization objects without error.
 */
TEST_F(SyncObjectsDeviceTest, FrameSync_Create)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_THROW({
		FrameSync fs(*m_device);
		EXPECT_TRUE(*fs.inFlight.handle());
		EXPECT_TRUE(*fs.imageAvailable.handle());
		EXPECT_TRUE(*fs.renderFinished.handle());
	});
}

/**
 * @test FrameSync fence is signaled by default, so WaitAndReset returns true.
 */
TEST_F(SyncObjectsDeviceTest, FrameSync_FenceInitiallySignaled)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	FrameSync fs(*m_device);
	EXPECT_TRUE(fs.inFlight.WaitAndReset(0));
}
