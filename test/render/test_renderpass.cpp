/**
 * @file test_renderpass.cpp
 * @brief Tests for Pass type queries — static methods moved from RenderPassManager.
 *
 * All tests here are compile-time / CPU-only; no Vulkan GPU required.
 * They verifies PassType enum values, ColorAttachmentCount, HasDepth,
 * and PresetClearValues for every pass type.
 */

#include <gtest/gtest.h>

#include "render/passes/Pass.hpp"

using namespace neurus;

// ---------------------------------------------------------------------------
// Pass Type Enum Values
// ---------------------------------------------------------------------------

TEST(PassTypeTest, EnumValuesExist)
{
	// Verify all pass type enum values are defined
	auto gBuffer   = Pass::PassType::G_BUFFER;
	auto lighting  = Pass::PassType::LIGHTING;
	auto shadow    = Pass::PassType::SHADOW;
	auto composite = Pass::PassType::COMPOSITE;
	auto postFx    = Pass::PassType::POST_FX;

	// Just ensure they compile and are distinct
	EXPECT_NE(static_cast<int>(gBuffer), static_cast<int>(lighting));
	EXPECT_NE(static_cast<int>(lighting), static_cast<int>(shadow));
	EXPECT_NE(static_cast<int>(shadow), static_cast<int>(composite));
	EXPECT_NE(static_cast<int>(composite), static_cast<int>(postFx));
}

// ---------------------------------------------------------------------------
// Color Attachment Count per Pass Type
// ---------------------------------------------------------------------------

TEST(PassTypeTest, ColorAttachmentCount_PerPassType)
{
	EXPECT_EQ(Pass::ColorAttachmentCount(Pass::PassType::G_BUFFER), 5u);
	EXPECT_EQ(Pass::ColorAttachmentCount(Pass::PassType::LIGHTING), 1u);
	EXPECT_EQ(Pass::ColorAttachmentCount(Pass::PassType::SHADOW), 0u);
	EXPECT_EQ(Pass::ColorAttachmentCount(Pass::PassType::COMPOSITE), 1u);
	EXPECT_EQ(Pass::ColorAttachmentCount(Pass::PassType::POST_FX), 1u);
}

// ---------------------------------------------------------------------------
// Depth Attachment Presence per Pass Type
// ---------------------------------------------------------------------------

TEST(PassTypeTest, HasDepth_PerPassType)
{
	EXPECT_TRUE (Pass::HasDepth(Pass::PassType::G_BUFFER));
	EXPECT_FALSE(Pass::HasDepth(Pass::PassType::LIGHTING));
	EXPECT_TRUE (Pass::HasDepth(Pass::PassType::SHADOW));
	EXPECT_FALSE(Pass::HasDepth(Pass::PassType::COMPOSITE));
	EXPECT_FALSE(Pass::HasDepth(Pass::PassType::POST_FX));
}

// ---------------------------------------------------------------------------
// Preset Clear Values per Pass Type
// ---------------------------------------------------------------------------

TEST(PassTypeTest, PresetClearValues_G_Buffer)
{
	const auto clearValues = Pass::PresetClearValues(Pass::PassType::G_BUFFER);

	// G_BUFFER: 5 color clear values + 1 depth clear value = 6
	ASSERT_EQ(clearValues.size(), 6u);

	// All color clear values should be black
	for (size_t i = 0; i < 5; ++i)
	{
		EXPECT_EQ(clearValues[i].color.float32[0], 0.0f);
		EXPECT_EQ(clearValues[i].color.float32[1], 0.0f);
		EXPECT_EQ(clearValues[i].color.float32[2], 0.0f);
		EXPECT_EQ(clearValues[i].color.float32[3], 0.0f);
	}

	// Depth clear value: 1.0f (far plane)
	EXPECT_FLOAT_EQ(clearValues[5].depthStencil.depth, 1.0f);
	EXPECT_EQ(clearValues[5].depthStencil.stencil, 0u);
}

TEST(PassTypeTest, PresetClearValues_Lighting)
{
	const auto clearValues = Pass::PresetClearValues(Pass::PassType::LIGHTING);

	// LIGHTING: 1 color clear value, no depth
	ASSERT_EQ(clearValues.size(), 1u);

	EXPECT_FLOAT_EQ(clearValues[0].color.float32[0], 0.0f);
	EXPECT_FLOAT_EQ(clearValues[0].color.float32[1], 0.0f);
	EXPECT_FLOAT_EQ(clearValues[0].color.float32[2], 0.0f);
	EXPECT_FLOAT_EQ(clearValues[0].color.float32[3], 0.0f);
}

TEST(PassTypeTest, PresetClearValues_Shadow)
{
	const auto clearValues = Pass::PresetClearValues(Pass::PassType::SHADOW);

	// SHADOW: no color, 1 depth clear value only
	ASSERT_EQ(clearValues.size(), 1u);

	EXPECT_FLOAT_EQ(clearValues[0].depthStencil.depth, 1.0f);
	EXPECT_EQ(clearValues[0].depthStencil.stencil, 0u);
}

TEST(PassTypeTest, PresetClearValues_Composite)
{
	const auto clearValues = Pass::PresetClearValues(Pass::PassType::COMPOSITE);

	ASSERT_EQ(clearValues.size(), 1u);
	EXPECT_FLOAT_EQ(clearValues[0].color.float32[0], 0.0f);
}

TEST(PassTypeTest, PresetClearValues_PostFX)
{
	const auto clearValues = Pass::PresetClearValues(Pass::PassType::POST_FX);

	ASSERT_EQ(clearValues.size(), 1u);
	EXPECT_FLOAT_EQ(clearValues[0].color.float32[0], 0.0f);
}
