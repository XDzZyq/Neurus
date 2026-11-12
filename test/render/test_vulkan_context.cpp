// Must define platform before including Vulkan headers

#include <gtest/gtest.h>

#include "app/VulkanContext.h"
#include "platform/PlatformSurface.h"

using namespace neurus;

/**
 * @brief Tests for VulkanContext - instance creation, device selection, and cleanup.
 *
 * @note These tests require a Vulkan 1.4-capable GPU. They will be skipped
 *       in CI environments without GPU access by checking for available devices.
 */
class VulkanContextTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// Check Vulkan availability
		try
		{
			auto instance = VulkanContext::CreateInstance();
			auto physicalDevices = vk::raii::PhysicalDevices(instance);
			m_hasVulkan = !physicalDevices.empty();
		}
		catch (...)
		{
			m_hasVulkan = false;
		}
	}

	void TearDown() override
	{
	}

	bool m_hasVulkan = false;
};

TEST_F(VulkanContextTest, CreateInstance_Succeeds)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_THROW({
		auto instance = VulkanContext::CreateInstance();
		ASSERT_TRUE(*instance);
	});
}

TEST_F(VulkanContextTest, CreateInstance_HasRequiredExtensions)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto instance = VulkanContext::CreateInstance();

	// Verify required extensions are available
	auto extensions = vk::enumerateInstanceExtensionProperties();
	bool hasSurface = false;
	bool hasPlatformSurface = false;

	auto requiredExts = neurus::platform::RequiredInstanceExtensions();

	for (const auto& ext : extensions)
	{
		if (strcmp(ext.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0)
		{
			hasSurface = true;
		}
		// Check the platform-specific surface extension (second in the list)
		if (requiredExts.size() > 1 && strcmp(ext.extensionName, requiredExts[1]) == 0)
		{
			hasPlatformSurface = true;
		}
	}

	EXPECT_TRUE(hasSurface) << "VK_KHR_surface not supported";
	EXPECT_TRUE(hasPlatformSurface) << "Platform surface extension not supported";
}

TEST_F(VulkanContextTest, InstanceCleanup_NoCrash)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// Instance should clean up on destruction without errors
	{
		auto instance = VulkanContext::CreateInstance();
	}
	// If we get here without crash, cleanup worked
	SUCCEED();
}

TEST_F(VulkanContextTest, InitDevice_UploadsUseTheGraphicsFamily)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	VulkanContext ctx(VulkanContext::CreateInstance());
	ctx.InitDevice();

	// Uploads are synchronous on the render thread, so they ride the graphics
	// queue: a separate family would need queue-family ownership transfers on
	// every image it writes, and the upload barriers name fragment stages a
	// compute-only queue does not support. The rule is not MoltenVK's single
	// family - it must hold on a device that has several.
	EXPECT_EQ(ctx.transferQueueFamily(), ctx.graphicsQueueFamily());
}
