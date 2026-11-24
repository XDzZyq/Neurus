#include <gtest/gtest.h>

#include "asset/data/ImageData.hpp"
#include "asset/PixelFormat.hpp"

#include <numeric>
#include <vector>

using namespace neurus;

TEST(ImageDataTest, PixelFormatMetadata)
{
	struct FormatCase
	{
		const char* name;
		PixelFormat format;
		uint32_t bytes;
		uint32_t channels;
		bool bgr;
	};
	constexpr FormatCase cases[] = {
		{"RGBA8U", PixelFormat::RGBA8U, 4, 4, false},
		{"RGBA8S", PixelFormat::RGBA8S, 4, 4, false},
		{"BGRA8U", PixelFormat::BGRA8U, 4, 4, true},
		{"BGRA8S", PixelFormat::BGRA8S, 4, 4, true},
		{"RGBA16F", PixelFormat::RGBA16F, 8, 4, false},
		{"RGBA16U", PixelFormat::RGBA16U, 8, 4, false},
		{"RGBA16SN", PixelFormat::RGBA16SN, 8, 4, false},
		{"R8U", PixelFormat::R8U, 1, 1, false},
		{"R8S", PixelFormat::R8S, 1, 1, false},
		{"Undefined", PixelFormat::Undefined, 0, 4, false},
	};
	for (const auto& sample : cases)
	{
		SCOPED_TRACE(sample.name);
		EXPECT_EQ(PixelByteSize(sample.format), sample.bytes);
		EXPECT_EQ(ChannelCount(sample.format), sample.channels);
		EXPECT_EQ(IsBGRFormat(sample.format), sample.bgr);
	}
}

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

TEST(ImageDataTest, ConstructorPreservesDimensionsFormatAndPixels)
{
	std::vector<uint8_t> pixels(3 * 2 * 4);
	std::iota(pixels.begin(), pixels.end(), uint8_t{1});
	ImageData image(pixels.data(), 3, 2, PixelFormat::RGBA8S);

	EXPECT_EQ(image.GetWidth(), 3u);
	EXPECT_EQ(image.GetHeight(), 2u);
	EXPECT_EQ(image.GetFormat(), PixelFormat::RGBA8S);
	EXPECT_EQ(image.GetPixelData(), pixels);
}
