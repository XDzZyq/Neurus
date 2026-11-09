/**
 * @file test_pickpixel.cpp
 * @brief GPU tests for Image::PickPixel — single-pixel GPU→CPU readback.
 *
 * Tests verify that PickPixel correctly copies a single pixel from a GPU
 * image to host memory via staging buffer, including readback at corners
 * and bounds-checking for out-of-range coordinates.
 */

// Must define platform before including Vulkan headers


#include "shared/TestVulkanShared.h"

#include <gtest/gtest.h>

#include "render/Image.h"
#include "render/Barrier.h"

#include <vulkan/vulkan_raii.hpp>

#include <array>
#include <cstring>
#include <vector>

using namespace neurus;

/**
 * @brief Test fixture for PickPixel GPU tests.
 *
 * Inherits the standard Vulkan bootstrap from VulkanTestShared
 * (Instance → Device → Queue → CommandPool).
 */
class PickPixelTest : public VulkanTestShared
{
protected:
	/**
	 * @brief Creates a GPU Image filled with known RGBA8 pixel data.
	 *
	 * Uploads @p pixelData via a staging buffer, transitions the image
	 * to ColorShaderRead, and returns the Image.
	 *
	 * @param width    Image width in pixels.
	 * @param height   Image height in pixels.
	 * @param pixelData RGBA8 pixel data (width * height * 4 bytes), row-major.
	 * @return Populated Image in ColorShaderRead state.
	 */
	Image CreateTestImageRGBA8(uint32_t width,
	                           uint32_t height,
	                           const std::vector<uint8_t>& pixelData)
	{
		auto& pd = PhysicalDevice();
		const vk::Extent2D extent(width, height);

		Image image(*m_device, pd, extent,
		            vk::Format::eR8G8B8A8Unorm,
		            vk::ImageUsageFlagBits::eTransferDst |
		                vk::ImageUsageFlagBits::eTransferSrc,
		            1, Image::ImageType::e2D, "PickPixelTestRGBA8");

		const vk::DeviceSize bufferSize = static_cast<vk::DeviceSize>(pixelData.size());

		// --- Staging buffer ---
		vk::BufferCreateInfo stagingCI({}, bufferSize,
		                               vk::BufferUsageFlagBits::eTransferSrc);
		vk::raii::Buffer stagingBuf(*m_device, stagingCI);

		auto stagingReqs = stagingBuf.getMemoryRequirements();
		const uint32_t stagingMemType = FindMemoryType(
			pd, stagingReqs.memoryTypeBits,
			vk::MemoryPropertyFlagBits::eHostVisible |
			    vk::MemoryPropertyFlagBits::eHostCoherent);
		vk::MemoryAllocateInfo stagingAlloc(stagingReqs.size, stagingMemType);
		vk::raii::DeviceMemory stagingMem(*m_device, stagingAlloc);
		stagingBuf.bindMemory(*stagingMem, 0);

		void* mapped = stagingMem.mapMemory(0, bufferSize);
		std::memcpy(mapped, pixelData.data(), static_cast<size_t>(bufferSize));
		stagingMem.unmapMemory();

		// --- Upload ---
		auto& cmd = BeginCmd();

		Barrier::Transition(*cmd, image, ImageState::TransferDst);

		vk::BufferImageCopy copyRegion;
		copyRegion.bufferOffset      = 0;
		copyRegion.bufferRowLength   = 0;
		copyRegion.bufferImageHeight = 0;
		copyRegion.imageSubresource  = vk::ImageSubresourceLayers(
			vk::ImageAspectFlagBits::eColor, 0, 0, 1);
		copyRegion.imageOffset = vk::Offset3D(0, 0, 0);
		copyRegion.imageExtent = vk::Extent3D(width, height, 1);

		cmd.copyBufferToImage(*stagingBuf, *image.ImageHandle(),
		                      vk::ImageLayout::eTransferDstOptimal, { copyRegion });

		Barrier::Transition(*cmd, image, ImageState::ColorShaderRead);

		EndSubmitWait(cmd);

		return image;
	}

	/**
	 * @brief Creates a 4×4 RGBA8 test image with a checkerboard pattern.
	 *
	 * Pixel values:
	 *   (0,0): RGBA(  1,   2,   3, 255)   (1,0): RGBA(  4,   5,   6, 255) ...
	 *   (0,1): RGBA( 17,  18,  19, 255)   (1,1): RGBA( 20,  21,  22, 255) ...
	 *   ...
	 *
	 * Row-major: pixel at (x, y) has value = (y * 16 + x * 4 + channel, ...)
	 * This gives each pixel a unique, predictable RGBA value.
	 */
	Image CreateCheckerboard4x4()
	{
		constexpr uint32_t kWidth  = 4;
		constexpr uint32_t kHeight = 4;
		std::vector<uint8_t> pixels(kWidth * kHeight * 4);

		for (uint32_t y = 0; y < kHeight; ++y)
		{
			for (uint32_t x = 0; x < kWidth; ++x)
			{
				const uint32_t base = (y * kWidth + x) * 4;
				pixels[base + 0] = static_cast<uint8_t>(base + 1);  // R
				pixels[base + 1] = static_cast<uint8_t>(base + 2);  // G
				pixels[base + 2] = static_cast<uint8_t>(base + 3);  // B
				pixels[base + 3] = 255;                              // A
			}
		}

		return CreateTestImageRGBA8(kWidth, kHeight, pixels);
	}

	/**
	 * @brief Returns the expected RGBA8 pixel value at (x, y) for the
	 *        checkerboard pattern created by CreateCheckerboard4x4().
	 */
	static std::array<uint8_t, 4> CheckerboardPixel(uint32_t x, uint32_t y)
	{
		const uint32_t base = (y * 4 + x) * 4;
		return {
			static_cast<uint8_t>(base + 1),
			static_cast<uint8_t>(base + 2),
			static_cast<uint8_t>(base + 3),
			255
		};
	}
};

// ---------------------------------------------------------------------------
// Basic pixel readback
// ---------------------------------------------------------------------------

TEST_F(PickPixelTest, ReadbackAtOrigin)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto image = CreateCheckerboard4x4();

	auto bytes = image.PickPixel(*m_device, PhysicalDevice(),
	                             *m_queue, m_graphicsQueueFamily, 0, 0);

	ASSERT_EQ(bytes.size(), 4u) << "RGBA8 = 4 bytes per pixel";
	const auto expected = CheckerboardPixel(0, 0);
	EXPECT_EQ(bytes[0], expected[0]) << "R at (0,0)";
	EXPECT_EQ(bytes[1], expected[1]) << "G at (0,0)";
	EXPECT_EQ(bytes[2], expected[2]) << "B at (0,0)";
	EXPECT_EQ(bytes[3], expected[3]) << "A at (0,0)";
}

TEST_F(PickPixelTest, ReadbackAtBottomRight)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto image = CreateCheckerboard4x4();

	auto bytes = image.PickPixel(*m_device, PhysicalDevice(),
	                             *m_queue, m_graphicsQueueFamily, 3, 3);

	ASSERT_EQ(bytes.size(), 4u);
	const auto expected = CheckerboardPixel(3, 3);
	EXPECT_EQ(bytes[0], expected[0]) << "R at (3,3)";
	EXPECT_EQ(bytes[1], expected[1]) << "G at (3,3)";
	EXPECT_EQ(bytes[2], expected[2]) << "B at (3,3)";
	EXPECT_EQ(bytes[3], expected[3]) << "A at (3,3)";
}

TEST_F(PickPixelTest, ReadbackAllPixels)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto image = CreateCheckerboard4x4();

	for (uint32_t y = 0; y < 4; ++y)
	{
		for (uint32_t x = 0; x < 4; ++x)
		{
			auto bytes = image.PickPixel(*m_device, PhysicalDevice(),
			                             *m_queue, m_graphicsQueueFamily, x, y);
			ASSERT_EQ(bytes.size(), 4u)
				<< "Pixel (" << x << ", " << y << ") should be 4 bytes";
			const auto expected = CheckerboardPixel(x, y);
			EXPECT_EQ(bytes[0], expected[0])
				<< "R mismatch at (" << x << ", " << y << ")";
			EXPECT_EQ(bytes[1], expected[1])
				<< "G mismatch at (" << x << ", " << y << ")";
			EXPECT_EQ(bytes[2], expected[2])
				<< "B mismatch at (" << x << ", " << y << ")";
			EXPECT_EQ(bytes[3], expected[3])
				<< "A mismatch at (" << x << ", " << y << ")";
		}
	}
}

// ---------------------------------------------------------------------------
// Bounds checking
// ---------------------------------------------------------------------------

TEST_F(PickPixelTest, OutOfBoundsReturnsEmpty)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto image = CreateCheckerboard4x4();

	// x out of bounds
	{
		auto bytes = image.PickPixel(*m_device, PhysicalDevice(),
		                             *m_queue, m_graphicsQueueFamily, 4, 0);
		EXPECT_TRUE(bytes.empty()) << "x=4 out of bounds for 4-wide image";
	}

	// y out of bounds
	{
		auto bytes = image.PickPixel(*m_device, PhysicalDevice(),
		                             *m_queue, m_graphicsQueueFamily, 0, 4);
		EXPECT_TRUE(bytes.empty()) << "y=4 out of bounds for 4-tall image";
	}

	// Both out of bounds
	{
		auto bytes = image.PickPixel(*m_device, PhysicalDevice(),
		                             *m_queue, m_graphicsQueueFamily, 99, 99);
		EXPECT_TRUE(bytes.empty()) << "both coordinates out of bounds";
	}
}

// ---------------------------------------------------------------------------
// Repeat readback (verify no state corruption)
// ---------------------------------------------------------------------------

TEST_F(PickPixelTest, RepeatedReadbackSamePixel)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto image = CreateCheckerboard4x4();
	const auto expected = CheckerboardPixel(1, 2);

	// Read the same pixel 3 times — results should be identical
	for (int i = 0; i < 3; ++i)
	{
		auto bytes = image.PickPixel(*m_device, PhysicalDevice(),
		                             *m_queue, m_graphicsQueueFamily, 1, 2);
		ASSERT_EQ(bytes.size(), 4u) << "iteration " << i;
		EXPECT_EQ(bytes[0], expected[0]) << "R, iteration " << i;
		EXPECT_EQ(bytes[1], expected[1]) << "G, iteration " << i;
		EXPECT_EQ(bytes[2], expected[2]) << "B, iteration " << i;
		EXPECT_EQ(bytes[3], expected[3]) << "A, iteration " << i;
	}
}

// ---------------------------------------------------------------------------
// Never-written image: no readback, no barrier
// ---------------------------------------------------------------------------

TEST_F(PickPixelTest, NeverWrittenImageReturnsEmpty)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// A freshly created image is ImageState::Undefined - no pass has written it.
	const vk::Extent2D extent(4, 4);
	Image image(*m_device, PhysicalDevice(), extent,
	            vk::Format::eR8G8B8A8Unorm,
	            vk::ImageUsageFlagBits::eSampled |
	                vk::ImageUsageFlagBits::eTransferSrc,
	            1, Image::ImageType::e2D, "PickPixelNeverWritten");

	ASSERT_EQ(image.State(), ImageState::Undefined);

	auto bytes = image.PickPixel(*m_device, PhysicalDevice(),
	                             *m_queue, m_graphicsQueueFamily, 0, 0);

	// There is no pixel of an image that was never written. The pick must record
	// no GPU work at all: restoring the image to its "original" state would name
	// UNDEFINED as a barrier's newLayout, which is invalid
	// (VUID-VkImageMemoryBarrier2-newLayout-01198).
	EXPECT_TRUE(bytes.empty()) << "an unwritten image has nothing to read";
	EXPECT_EQ(image.State(), ImageState::Undefined)
		<< "a pick must not touch an image it did not read";
}

// ---------------------------------------------------------------------------
// A successful pick leaves the tracked state (and the layout) alone
// ---------------------------------------------------------------------------

TEST_F(PickPixelTest, ReadbackPreservesTrackedState)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto image = CreateCheckerboard4x4();
	ASSERT_EQ(image.State(), ImageState::ColorShaderRead);

	auto bytes = image.PickPixel(*m_device, PhysicalDevice(),
	                             *m_queue, m_graphicsQueueFamily, 1, 2);
	ASSERT_EQ(bytes.size(), 4u);

	EXPECT_EQ(image.State(), ImageState::ColorShaderRead)
		<< "PickPixel restores the image in the same submission";

	// The restore has to put the layout back where the state says it is, or the
	// next transition is recorded against a layout the image is not in. Read
	// again: same pixel, and still no state change.
	auto again = image.PickPixel(*m_device, PhysicalDevice(),
	                             *m_queue, m_graphicsQueueFamily, 1, 2);
	ASSERT_EQ(again.size(), 4u);
	EXPECT_EQ(again, bytes) << "a restored image still holds its contents";
	EXPECT_EQ(image.State(), ImageState::ColorShaderRead);
}

// ---------------------------------------------------------------------------
// RGBA32F pixel readback
// ---------------------------------------------------------------------------

TEST_F(PickPixelTest, ReadbackRGBA32F)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();
	const vk::Extent2D extent(2, 2);

	// Create a 2×2 RGBA32F image
	Image image(*m_device, pd, extent,
	            vk::Format::eR32G32B32A32Sfloat,
	            vk::ImageUsageFlagBits::eTransferDst |
	                vk::ImageUsageFlagBits::eTransferSrc,
	            1, Image::ImageType::e2D, "PickPixelTestRGBA32F");

	// Upload known float values: each pixel = (x, y, x+y, 1.0)
	// 4 pixels × 16 bytes = 64 bytes
	std::vector<float> uploadData(2 * 2 * 4);
	for (uint32_t y = 0; y < 2; ++y)
	{
		for (uint32_t x = 0; x < 2; ++x)
		{
			const uint32_t base = (y * 2 + x) * 4;
			uploadData[base + 0] = static_cast<float>(x);       // R = x
			uploadData[base + 1] = static_cast<float>(y);       // G = y
			uploadData[base + 2] = static_cast<float>(x + y);   // B = x + y
			uploadData[base + 3] = 1.0f;                        // A = 1.0
		}
	}

	const vk::DeviceSize bufferSize = uploadData.size() * sizeof(float);

	// Staging buffer
	vk::BufferCreateInfo stagingCI({}, bufferSize,
	                               vk::BufferUsageFlagBits::eTransferSrc);
	vk::raii::Buffer stagingBuf(*m_device, stagingCI);
	auto stagingReqs = stagingBuf.getMemoryRequirements();
	const uint32_t stagingMemType = FindMemoryType(
		pd, stagingReqs.memoryTypeBits,
		vk::MemoryPropertyFlagBits::eHostVisible |
		    vk::MemoryPropertyFlagBits::eHostCoherent);
	vk::MemoryAllocateInfo stagingAlloc(stagingReqs.size, stagingMemType);
	vk::raii::DeviceMemory stagingMem(*m_device, stagingAlloc);
	stagingBuf.bindMemory(*stagingMem, 0);

	void* mapped = stagingMem.mapMemory(0, bufferSize);
	std::memcpy(mapped, uploadData.data(), static_cast<size_t>(bufferSize));
	stagingMem.unmapMemory();

	// Upload to image
	auto& cmd = BeginCmd();
	Barrier::Transition(*cmd, image, ImageState::TransferDst);
	vk::BufferImageCopy copyRegion;
	copyRegion.imageSubresource = vk::ImageSubresourceLayers(
		vk::ImageAspectFlagBits::eColor, 0, 0, 1);
	copyRegion.imageExtent = vk::Extent3D(2, 2, 1);
	cmd.copyBufferToImage(*stagingBuf, *image.ImageHandle(),
	                      vk::ImageLayout::eTransferDstOptimal, { copyRegion });
	Barrier::Transition(*cmd, image, ImageState::ColorShaderRead);
	EndSubmitWait(cmd);

	// Read back pixel (1, 1) — should be R=1.0, G=1.0, B=2.0, A=1.0
	auto bytes = image.PickPixel(*m_device, pd,
	                             *m_queue, m_graphicsQueueFamily, 1, 1);

	// RGBA32F = 16 bytes per pixel
	ASSERT_EQ(bytes.size(), 16u);

	// Interpret bytes as 4 floats
	float result[4];
	std::memcpy(result, bytes.data(), sizeof(result));

	EXPECT_FLOAT_EQ(result[0], 1.0f);  // R = x = 1
	EXPECT_FLOAT_EQ(result[1], 1.0f);  // G = y = 1
	EXPECT_FLOAT_EQ(result[2], 2.0f);  // B = x + y = 2
	EXPECT_FLOAT_EQ(result[3], 1.0f);  // A = 1.0
}
