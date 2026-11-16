#include <gtest/gtest.h>

#include <vulkan/vulkan_raii.hpp>

#include "asset/data/ImageData.hpp"
#include "asset/PixelFormat.hpp"

#include <vector>

using namespace neurus;

// ---------------------------------------------------------------------------
// 1. PixelByteSize
// ---------------------------------------------------------------------------

TEST(ImageDataTest, PixelByteSize_RGBA8_Returns4)
{
	EXPECT_EQ(neurus::PixelByteSize(PixelFormat::RGBA8U), 4u);
	EXPECT_EQ(neurus::PixelByteSize(PixelFormat::RGBA8S), 4u);
	EXPECT_EQ(neurus::PixelByteSize(PixelFormat::BGRA8U), 4u);
	EXPECT_EQ(neurus::PixelByteSize(PixelFormat::BGRA8S), 4u);
}

TEST(ImageDataTest, PixelByteSize_RGBA16F_Returns8)
{
	EXPECT_EQ(neurus::PixelByteSize(PixelFormat::RGBA16F), 8u);
	EXPECT_EQ(neurus::PixelByteSize(PixelFormat::RGBA16U), 8u);
	EXPECT_EQ(neurus::PixelByteSize(PixelFormat::RGBA16SN), 8u);
}

TEST(ImageDataTest, PixelByteSize_R8_Returns1)
{
	EXPECT_EQ(neurus::PixelByteSize(PixelFormat::R8U), 1u);
	EXPECT_EQ(neurus::PixelByteSize(PixelFormat::R8S), 1u);
}

TEST(ImageDataTest, PixelByteSize_UnknownFormat_Returns0)
{
	EXPECT_EQ(neurus::PixelByteSize(PixelFormat::Undefined), 0u);
}

// ---------------------------------------------------------------------------
// 2. ChannelCount
// ---------------------------------------------------------------------------

TEST(ImageDataTest, ChannelCount_R8_Returns1)
{
	EXPECT_EQ(neurus::ChannelCount(PixelFormat::R8U), 1u);
	EXPECT_EQ(neurus::ChannelCount(PixelFormat::R8S), 1u);
}

TEST(ImageDataTest, ChannelCount_RGBA_Returns4)
{
	EXPECT_EQ(neurus::ChannelCount(PixelFormat::RGBA8U), 4u);
	EXPECT_EQ(neurus::ChannelCount(PixelFormat::RGBA8S), 4u);
	EXPECT_EQ(neurus::ChannelCount(PixelFormat::BGRA8U), 4u);
	EXPECT_EQ(neurus::ChannelCount(PixelFormat::BGRA8S), 4u);
	EXPECT_EQ(neurus::ChannelCount(PixelFormat::RGBA16F), 4u);
	EXPECT_EQ(neurus::ChannelCount(PixelFormat::Undefined), 4u);
}

// ---------------------------------------------------------------------------
// 3. IsBGRFormat
// ---------------------------------------------------------------------------

TEST(ImageDataTest, IsBGRFormat_BGRA_ReturnsTrue)
{
	EXPECT_TRUE(neurus::IsBGRFormat(PixelFormat::BGRA8U));
	EXPECT_TRUE(neurus::IsBGRFormat(PixelFormat::BGRA8S));
}

TEST(ImageDataTest, IsBGRFormat_RGBA_ReturnsFalse)
{
	EXPECT_FALSE(neurus::IsBGRFormat(PixelFormat::RGBA8U));
	EXPECT_FALSE(neurus::IsBGRFormat(PixelFormat::RGBA8S));
	EXPECT_FALSE(neurus::IsBGRFormat(PixelFormat::RGBA16F));
	EXPECT_FALSE(neurus::IsBGRFormat(PixelFormat::R8U));
}

// ---------------------------------------------------------------------------
// 4. ConvertHalfToU8 (remapSigned=false)
// ---------------------------------------------------------------------------

TEST(ImageDataTest, ConvertHalfToU8_Unmapped)
{
	// 2x2 image in RGBA16F format
	//
	// clang-format off
	// half-float encoding:
	//   0x3C00 = 1.0    0x3800 = 0.5    0x0000 = 0.0
	//   0x3400 = 0.25   0x3A00 = 0.75
	const uint16_t halfData[] = {
		0x3C00, 0x3800, 0x0000, 0x3C00,  // pixel 0: (1.0, 0.5, 0.0, 1.0)
		0x0000, 0x3800, 0x3C00, 0x3C00,  // pixel 1: (0.0, 0.5, 1.0, 1.0)
		0x3400, 0x3A00, 0x0000, 0x3800,  // pixel 2: (0.25, 0.75, 0.0, 0.5)
		0x0000, 0x0000, 0x0000, 0x0000,  // pixel 3: (0.0, 0.0, 0.0, 0.0)
	};
	// clang-format on

	auto result = ImageData::ConvertHalfToU8(halfData, 2, 2, false);

	ASSERT_EQ(result.size(), 2u * 2u * 4u);

	// Pixel 0: (1.0, 0.5, 0.0, 1.0) → (255, 128, 0, 255)
	EXPECT_EQ(result[0], 255);
	EXPECT_EQ(result[1], 128);
	EXPECT_EQ(result[2], 0);
	EXPECT_EQ(result[3], 255);

	// Pixel 1: (0.0, 0.5, 1.0, 1.0) → (0, 128, 255, 255)
	EXPECT_EQ(result[4], 0);
	EXPECT_EQ(result[5], 128);
	EXPECT_EQ(result[6], 255);
	EXPECT_EQ(result[7], 255);

	// Pixel 2: (0.25, 0.75, 0.0, 0.5) → (64, 191, 0, 255) — non-background, alpha forced opaque
	EXPECT_EQ(result[8], 64);
	EXPECT_EQ(result[9], 191);
	EXPECT_EQ(result[10], 0);
	EXPECT_EQ(result[11], 255);

	// Pixel 3: (0.0, 0.0, 0.0, 0.0) → (0, 0, 0, 0)
	EXPECT_EQ(result[12], 0);
	EXPECT_EQ(result[13], 0);
	EXPECT_EQ(result[14], 0);
	EXPECT_EQ(result[15], 0);
}

// ---------------------------------------------------------------------------
// 5. ConvertHalfToU8 (remapSigned=true)  —  normal-map style remap
// ---------------------------------------------------------------------------

TEST(ImageDataTest, ConvertHalfToU8_RemapSigned)
{
	// 2x2 signed values (e.g. view-space normals stored in RGBA16F)
	//
	// clang-format off
	// half-float encoding:
	//   0x3800 = 0.5    0xB800 = -0.5   0x3C00 = 1.0
	//   0xBC00 = -1.0   0x0000 = 0.0
	const uint16_t halfData[] = {
		0x3800, 0xB800, 0x3C00, 0x0000,  // pixel 0: (0.5, -0.5, 1.0, 0.0) — NOT background
		0xBC00, 0x0000, 0x3800, 0x3C00,  // pixel 1: (-1.0, 0.0, 0.5, 1.0) — NOT background
		0x0000, 0x0000, 0x0000, 0x0000,  // pixel 2: (0.0, 0.0, 0.0, 0.0) — BACKGROUND
		0x0000, 0x0000, 0x0000, 0x0000,  // pixel 3: (0.0, 0.0, 0.0, 0.0) — BACKGROUND
	};
	// clang-format on

	auto result = ImageData::ConvertHalfToU8(halfData, 2, 2, true);

	ASSERT_EQ(result.size(), 2u * 2u * 4u);

	// Pixel 0: 0.5→(0.5+1)*0.5=0.75→191,  -0.5→(-0.5+1)*0.5=0.25→64,  1.0→(1+1)*0.5=1.0→255,  alpha forced→255
	EXPECT_EQ(result[0], 191);
	EXPECT_EQ(result[1], 64);
	EXPECT_EQ(result[2], 255);
	EXPECT_EQ(result[3], 255);

	// Pixel 1: -1.0→(-1+1)*0.5=0→0,  0.0→(0+1)*0.5=0.5→128,  0.5→(0.5+1)*0.5=0.75→191,  alpha forced→255
	EXPECT_EQ(result[4], 0);
	EXPECT_EQ(result[5], 128);
	EXPECT_EQ(result[6], 191);
	EXPECT_EQ(result[7], 255);

	// Pixel 2: background — all channels 0, alpha 0
	EXPECT_EQ(result[8], 0);
	EXPECT_EQ(result[9], 0);
	EXPECT_EQ(result[10], 0);
	EXPECT_EQ(result[11], 0);

	// Pixel 3: background — all zero half values, alpha 0
	EXPECT_EQ(result[12], 0);
	EXPECT_EQ(result[13], 0);
	EXPECT_EQ(result[14], 0);
	EXPECT_EQ(result[15], 0);
}

// ---------------------------------------------------------------------------
// 6. SwizzleBGRtoRGB
// ---------------------------------------------------------------------------

TEST(ImageDataTest, SwizzleBGRtoRGB_4Channel)
{
	// 2x2 image stored in B,G,R,A byte order
	uint8_t data[] = {
		3, 2, 1, 4,      // B=3, G=2, R=1, A=4
		7, 5, 6, 8,      // B=7, G=5, R=6, A=8
		11, 10, 9, 12,   // B=11, G=10, R=9, A=12
		15, 14, 13, 16,  // B=15, G=14, R=13, A=16
	};

	ImageData::SwizzleBGRtoRGB(data, 2, 2, 4);

	// After swizzle: [R,G,B,A, ...]
	EXPECT_EQ(data[0], 1);
	EXPECT_EQ(data[1], 2);
	EXPECT_EQ(data[2], 3);
	EXPECT_EQ(data[3], 4);

	EXPECT_EQ(data[4], 6);
	EXPECT_EQ(data[5], 5);
	EXPECT_EQ(data[6], 7);
	EXPECT_EQ(data[7], 8);

	EXPECT_EQ(data[8], 9);
	EXPECT_EQ(data[9], 10);
	EXPECT_EQ(data[10], 11);
	EXPECT_EQ(data[11], 12);

	EXPECT_EQ(data[12], 13);
	EXPECT_EQ(data[13], 14);
	EXPECT_EQ(data[14], 15);
	EXPECT_EQ(data[15], 16);
}

TEST(ImageDataTest, SwizzleBGRtoRGB_3Channel)
{
	// 2x1 RGB image stored in B,G,R order (channels=3)
	uint8_t data[] = {
		10, 20, 30,  // B=10, G=20, R=30
		40, 50, 60,  // B=40, G=50, R=60
	};

	ImageData::SwizzleBGRtoRGB(data, 2, 1, 3);

	// After swizzle: [R,G,B, ...]
	EXPECT_EQ(data[0], 30);
	EXPECT_EQ(data[1], 20);
	EXPECT_EQ(data[2], 10);

	EXPECT_EQ(data[3], 60);
	EXPECT_EQ(data[4], 50);
	EXPECT_EQ(data[5], 40);
}

// ---------------------------------------------------------------------------
// 7. Constructor and getters
// ---------------------------------------------------------------------------

TEST(ImageDataTest, ConstructorAndGetters)
{
	const uint8_t pixelData[] = {255, 0, 0, 255};
	ImageData img(pixelData, 1, 1, PixelFormat::RGBA8U);

	EXPECT_EQ(img.GetWidth(), 1u);
	EXPECT_EQ(img.GetHeight(), 1u);
	EXPECT_EQ(img.GetFormat(), PixelFormat::RGBA8U);
	EXPECT_EQ(img.GetPixelData()[0], 255u);
}

TEST(ImageDataTest, Constructor_NonSquare)
{
	const std::vector<uint8_t> pixelData(4 * 4 * 4, 0); // 4 wide x 4 tall x RGBA
	ImageData img(pixelData.data(), 4, 4, PixelFormat::RGBA8S);

	EXPECT_EQ(img.GetWidth(), 4u);
	EXPECT_EQ(img.GetHeight(), 4u);
	EXPECT_EQ(img.GetFormat(), PixelFormat::RGBA8S);
	EXPECT_FALSE(img.GetPixelData().empty());
}
