/** @file test_renderpass.cpp
 *  @brief CPU checks for attachment declarations and clear values. */

#include <gtest/gtest.h>

#include "render/passes/Pass.hpp"

using namespace neurus;

TEST(PassTypeTest, AttachmentsAndClearValuesMatchPassType)
{
	using Type = Pass::PassType;
	struct PassCase
	{
		const char* name;
		Type type;
		size_t colors;
		bool depth;
	};
	constexpr PassCase cases[] = {
		{"geometry", Type::G_BUFFER, 5, true},
		{"lighting", Type::LIGHTING, 1, false},
		{"shadow", Type::SHADOW, 0, true},
		{"composite", Type::COMPOSITE, 1, false},
		{"post FX", Type::POST_FX, 1, false},
	};
	for (const auto& entry : cases)
	{
		SCOPED_TRACE(entry.name);
		EXPECT_EQ(Pass::ColorAttachmentCount(entry.type), entry.colors);
		EXPECT_EQ(Pass::HasDepth(entry.type), entry.depth);
		const auto values = Pass::PresetClearValues(entry.type);
		ASSERT_EQ(values.size(), entry.colors + entry.depth);
		for (size_t i = 0; i < entry.colors; ++i)
		{
			SCOPED_TRACE(i);
			for (float channel : values[i].color.float32)
				EXPECT_FLOAT_EQ(channel, 0.0f);
		}
		if (entry.depth)
		{
			EXPECT_FLOAT_EQ(values.back().depthStencil.depth, 1.0f);
			EXPECT_EQ(values.back().depthStencil.stencil, 0u);
		}
	}
}
