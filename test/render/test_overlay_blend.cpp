/**
 * @file test_overlay_blend.cpp
 * @brief The overlay seed: copy the scene tail into OverlayOutput and blend the
 *        selection highlight onto it, without touching the scene image.
 *
 * Why this test exists: the copy is what makes an overlay-only redraw possible.
 * If OverlayCopyPass ever wrote the scene tail in place, an overlay frame would
 * start from the previous overlay frame's pixels instead of the converged image,
 * and the highlight would accumulate into a solid block. Both halves are asserted
 * here: the blended texels, the untouched texels, and the scene image afterwards.
 *
 * The mask is primed directly rather than produced by SelectionOutlinePass: this
 * is a test of the copy + blend, and SelectionOutlinePass (IDBuffer -> mask) has
 * its own coverage. Priming keeps the fixture to one dispatch and no scene.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/Barrier.h"
#include "render/Image.h"
#include "render/RenderCache.h"
#include "render/RenderContext.h"
#include "render/overlay/OverlayEngine.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace neurus;

namespace {

constexpr uint32_t kW = 8;
constexpr uint32_t kH = 4;

/// Everything left of this column carries the highlight mask; the rest is clear.
constexpr uint32_t kMaskColumn = 3;

/// Scene colour every texel starts at (also its exact expected value where the
/// mask is 0 - the copy must be lossless there).
constexpr float kSceneR = 0.25f;
constexpr float kSceneG = 0.50f;
constexpr float kSceneB = 0.75f;
constexpr float kSceneA = 1.00f;

/// Values the shader blends toward (overlay_copy.comp), at half intensity.
constexpr float kHighlightR = 1.0f;
constexpr float kHighlightG = 0.55f;
constexpr float kHighlightB = 0.0f;
constexpr float kHighlightMix = 0.5f;

/// Tolerance for a half-float round trip plus the blend's rounding.
constexpr float kTolerance = 2e-3f;

class OverlayBlendTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;
		m_cache = std::make_unique<RenderCache>(*m_device, PhysicalDevice());
		m_pass  = std::make_unique<OverlayCopyPass>(*m_device, PhysicalDevice(), 2);
	}

	void TearDown() override
	{
		m_pass.reset();
		m_cache.reset();
		VulkanTestShared::TearDown();
	}

	vk::Extent2D Extent() const { return vk::Extent2D{kW, kH}; }

	/** @brief Fills the scene tail with one colour and the mask with a hard edge. */
	void PrimeAttachments()
	{
		auto& scene = m_cache->GetAttachment(m_pass->GetSource(), Extent());
		auto& mask  = m_cache->GetAttachment(AttachmentName::SelectionOutline, Extent());

		const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

		auto& c = BeginCmd();
		Barrier::Transition(*c, scene, ImageState::TransferDst);
		c.clearColorImage(*scene.ImageHandle(), vk::ImageLayout::eTransferDstOptimal,
		                  vk::ClearColorValue(std::array<float, 4>{
		                      kSceneR, kSceneG, kSceneB, kSceneA}),
		                  range);
		EndSubmitWait(c);

		// The mask needs a hard edge, which a clear cannot express: the blend is
		// mix(scene, highlight, mask), so 1.0 under the left columns and 0.0 right.
		std::vector<uint8_t> texels(static_cast<size_t>(kW) * kH, 0u);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kMaskColumn; ++x)
				texels[static_cast<size_t>(y) * kW + x] = 255u;

		const size_t bytes = texels.size();
		vk::BufferCreateInfo bufferCI({}, bytes, vk::BufferUsageFlagBits::eTransferSrc);
		vk::raii::Buffer staging(*m_device, bufferCI);
		const auto memReqs = staging.getMemoryRequirements();
		const uint32_t memType = FindMemoryType(PhysicalDevice(), memReqs.memoryTypeBits,
		                                        vk::MemoryPropertyFlagBits::eHostVisible |
		                                        vk::MemoryPropertyFlagBits::eHostCoherent);
		vk::raii::DeviceMemory stagingMem(*m_device,
		                                 vk::MemoryAllocateInfo(memReqs.size, memType));
		void* mapped = stagingMem.mapMemory(0, bytes);
		std::memcpy(mapped, texels.data(), bytes);
		stagingMem.unmapMemory();
		staging.bindMemory(*stagingMem, 0);

		auto& upload = BeginCmd();
		Barrier::Transition(*upload, mask, ImageState::TransferDst);
		const vk::BufferImageCopy copyRegion(
			0, 0, 0,
			vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
			vk::Offset3D(0, 0, 0),
			vk::Extent3D(kW, kH, 1));
		upload.copyBufferToImage(*staging, *mask.ImageHandle(),
		                         vk::ImageLayout::eTransferDstOptimal, copyRegion);
		EndSubmitWait(upload);
	}

	/** @brief Records the copy + blend dispatch over the primed attachments. */
	PassStats RunPass()
	{
		RenderContext ctx;
		ctx.width      = kW;
		ctx.height     = kH;
		ctx.frameIndex = 0;

		auto& c = BeginCmd();
		const PassStats stats = m_pass->Record(*c, *m_cache, ctx);
		EndSubmitWait(c);
		return stats;
	}

	/** @brief Reads an RGBA16F attachment back as floats, row-major. */
	struct Texel { float r, g, b, a; };

	std::vector<Texel> ReadColor(AttachmentName name)
	{
		auto& att = m_cache->GetAttachment(name, Extent());
		auto data = att.ReadImageData(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
		const auto* h = reinterpret_cast<const uint16_t*>(data->GetPixelData().data());

		std::vector<Texel> out(static_cast<size_t>(kW) * kH);
		for (size_t i = 0; i < out.size(); ++i)
		{
			out[i] = Texel{VulkanTestShared::HalfToFloat(h[i * 4 + 0]),
			               VulkanTestShared::HalfToFloat(h[i * 4 + 1]),
			               VulkanTestShared::HalfToFloat(h[i * 4 + 2]),
			               VulkanTestShared::HalfToFloat(h[i * 4 + 3])};
		}
		return out;
	}

	std::unique_ptr<RenderCache>     m_cache;
	std::unique_ptr<OverlayCopyPass> m_pass;
};

} // namespace

// =========================================================================
// Graph-facing contract
// =========================================================================

TEST_F(OverlayBlendTest, DeclaresSceneTailAndMaskAsInputsAndOverlayOutputAsItsWrite)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	const PassIO io = m_pass->GetIO();

	ASSERT_EQ(io.writes.size(), 1u);
	EXPECT_EQ(io.writes[0].resource, AttachmentName::OverlayOutput)
		<< "The overlay rasters draw over OverlayOutput, so the seed must write it";

	ASSERT_EQ(io.reads.size(), 2u);
	EXPECT_EQ(io.reads[0].resource, AttachmentName::ComposedOutput)
		<< "Default scene tail; DeferredRenderer re-points it via SetSource";
	EXPECT_EQ(io.reads[1].resource, AttachmentName::SelectionOutline);
}

TEST_F(OverlayBlendTest, SetSourceMovesTheCopyInput)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	EXPECT_EQ(m_pass->GetSource(), AttachmentName::ComposedOutput);
	m_pass->SetSource(AttachmentName::FXAAOutput);
	EXPECT_EQ(m_pass->GetSource(), AttachmentName::FXAAOutput);
	EXPECT_EQ(m_pass->GetIO().reads[0].resource, AttachmentName::FXAAOutput)
		<< "GetIO() must follow SetSource, or the graph would order the wrong edge";
	m_pass->SetSource(AttachmentName::ComposedOutput);
}

// =========================================================================
// Copy + blend
// =========================================================================

TEST_F(OverlayBlendTest, CopiesTheSceneAndBlendsTheMask)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments();
	const PassStats stats = RunPass();
	EXPECT_EQ(stats.dispatches, 1u) << "Copy and blend are one dispatch by design";

	const std::vector<Texel> out = ReadColor(AttachmentName::OverlayOutput);

	int blended = 0;
	int untouched = 0;
	for (uint32_t y = 0; y < kH; ++y)
	for (uint32_t x = 0; x < kW; ++x)
	{
		const Texel& t = out[static_cast<size_t>(y) * kW + x];
		const float mask = (x < kMaskColumn) ? 1.0f : 0.0f;

		const float expectedR = kSceneR + (kHighlightR - kSceneR) * kHighlightMix * mask;
		const float expectedG = kSceneG + (kHighlightG - kSceneG) * kHighlightMix * mask;
		const float expectedB = kSceneB + (kHighlightB - kSceneB) * kHighlightMix * mask;

		EXPECT_NEAR(t.r, expectedR, kTolerance) << "pixel (" << x << "," << y << ") R";
		EXPECT_NEAR(t.g, expectedG, kTolerance) << "pixel (" << x << "," << y << ") G";
		EXPECT_NEAR(t.b, expectedB, kTolerance) << "pixel (" << x << "," << y << ") B";
		EXPECT_NEAR(t.a, kSceneA, kTolerance) << "pixel (" << x << "," << y << ") alpha";

		if (mask > 0.0f) ++blended; else ++untouched;
	}

	// Both halves are present, so neither assertion could pass vacuously.
	EXPECT_EQ(blended, static_cast<int>(kMaskColumn) * kH);
	EXPECT_EQ(untouched, static_cast<int>(kW - kMaskColumn) * kH);
}

TEST_F(OverlayBlendTest, LeavesTheSceneImageUntouched)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	PrimeAttachments();
	RunPass();

	// The whole point of the copy: an overlay redraw must not consume the scene
	// image, or the next redraw would blend over a previous frame's highlight.
	const std::vector<Texel> scene = ReadColor(AttachmentName::ComposedOutput);
	for (size_t i = 0; i < scene.size(); ++i)
	{
		EXPECT_NEAR(scene[i].r, kSceneR, kTolerance) << "texel " << i << " R";
		EXPECT_NEAR(scene[i].g, kSceneG, kTolerance) << "texel " << i << " G";
		EXPECT_NEAR(scene[i].b, kSceneB, kTolerance) << "texel " << i << " B";
		EXPECT_NEAR(scene[i].a, kSceneA, kTolerance) << "texel " << i << " A";
	}
}

TEST_F(OverlayBlendTest, RedrawingTwiceIsIdempotent)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	// A second overlay frame starts from a fresh copy of the scene, so repeating
	// the pass must reproduce the same image instead of blending the highlight on
	// top of itself. This is the regression that would appear if the copy ever
	// wrote the scene tail in place.
	PrimeAttachments();
	RunPass();
	const std::vector<Texel> first = ReadColor(AttachmentName::OverlayOutput);

	RunPass();
	const std::vector<Texel> second = ReadColor(AttachmentName::OverlayOutput);

	for (size_t i = 0; i < first.size(); ++i)
	{
		EXPECT_NEAR(second[i].r, first[i].r, kTolerance) << "texel " << i << " R";
		EXPECT_NEAR(second[i].g, first[i].g, kTolerance) << "texel " << i << " G";
		EXPECT_NEAR(second[i].b, first[i].b, kTolerance) << "texel " << i << " B";
	}
}
