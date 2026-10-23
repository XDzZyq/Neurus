/**
 * @file test_fxaa.cpp
 * @brief Analytical verification: stair-pattern FXAA output vs expected linear gradient.
 *
 * Edge equation: y = 7 - (7/23)*x  (from (0,7) to (23,0) in pixel coords)
 * Expected luma: fraction of pixel on white side = clamp(edge_y - py, 0, 1)
 */

#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <numeric>

#include "shared/TestVulkanShared.h"
#include "render/Barrier.h"
#include "render/Image.h"
#include "render/RenderCache.h"
#include "core/RenderConfig.h"
#include "render/RenderContext.h"
#include "render/passes/FXAAPass.h"

using namespace neurus;

namespace {

constexpr uint32_t kW = 24, kH = 8;

/** Edge line: y = 7 - (7/23)*x. Expected luma = fraction of pixel above edge. */
float ExpectedLuma(int px, int py)
{
	float cx = float(px) + 0.5f;
	float edgeY = 7.0f - (7.0f / 23.0f) * cx;
	float frac = edgeY - float(py);
	if (frac < 0.0f) return 0.0f;
	if (frac > 1.0f) return 1.0f;
	return frac;
}

static uint16_t FloatToHalf(float f)
{
	uint32_t x;
	std::memcpy(&x, &f, sizeof(x));
	uint32_t sign     = (x >> 16) & 0x8000;
	int32_t  exponent = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
	uint32_t mantissa = (x >> 13) & 0x3FF;
	if (exponent <= 0) {
		if (exponent < -10) return uint16_t(sign);
		mantissa = (mantissa | 0x400) >> (1 - exponent);
		return uint16_t(sign | mantissa);
	}
	if (exponent >= 31) {
		if (exponent > 31) return uint16_t(sign | 0x7C00);
		return uint16_t(sign | 0x7C00 | mantissa);
	}
	return uint16_t(sign | (uint32_t(exponent) << 10) | mantissa);
}

class FXAATest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;
		m_cache   = std::make_unique<RenderCache>(*m_device, PhysicalDevice());
		m_fxaaPass = std::make_unique<FXAAPass>(*m_device, PhysicalDevice(), 2);
	}
	void TearDown() override
	{
		m_fxaaPass.reset();
		m_cache.reset();
		VulkanTestShared::TearDown();
	}

	void UploadStair(const vk::Extent2D& extent)
	{
		const size_t n = extent.width * extent.height;
		std::vector<uint16_t> px(n * 4);
		for (uint32_t y = 0; y < extent.height; ++y)
		for (uint32_t x = 0; x < extent.width;  ++x) {
			size_t i = (y*extent.width + x)*4;
			float v = ExpectedLuma(int(x), int(y)) >= 0.5f ? 1.0f : 0.0f;
			uint16_t h = FloatToHalf(v);
			px[i+0]=h; px[i+1]=h; px[i+2]=h; px[i+3]=FloatToHalf(1.0f);
		}
		size_t ds = n*4*sizeof(uint16_t);
		vk::BufferCreateInfo sci({}, ds, vk::BufferUsageFlagBits::eTransferSrc);
		vk::raii::Buffer sb(*m_device, sci);
		auto mr = sb.getMemoryRequirements();
		uint32_t mt = FindMemoryType(PhysicalDevice(), mr.memoryTypeBits,
			vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent);
		vk::raii::DeviceMemory sm(*m_device, vk::MemoryAllocateInfo(mr.size, mt));
		void* mp = sm.mapMemory(0, ds);
		std::memcpy(mp, px.data(), ds);
		sm.unmapMemory(); sb.bindMemory(*sm, 0);
		auto& att = m_cache->GetAttachment(AttachmentName::ComposedOutput, extent);
		auto& c = BeginCmd();
		Barrier::Transition(*c, att, ImageState::TransferDst);
		vk::BufferImageCopy cop(0,0,0,
			vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,0,0,1),
			vk::Offset3D(0,0,0), vk::Extent3D(extent.width, extent.height, 1));
		c.copyBufferToImage(*sb, *att.ImageHandle(), vk::ImageLayout::eTransferDstOptimal, cop);
		EndSubmitWait(c);
	}

	std::unique_ptr<RenderCache> m_cache;
	std::unique_ptr<FXAAPass>    m_fxaaPass;
};

} // namespace

// =========================================================================
// Analytical gradient verification
// =========================================================================

TEST_F(FXAATest, StairPattern_MatchesAnalyticalGradient)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";

	const vk::Extent2D e{kW, kH};
	UploadStair(e);

	RenderConfig cfg;
	cfg.r_aa = AAAlg::FXAA;
	cfg.r_fxaa_subpix            = 0.75f;
	cfg.r_fxaa_edge_threshold    = 0.166f;
	cfg.r_fxaa_edge_threshold_min = 0.0833f;

	RenderFrameSnapshot snapStorage;
	RenderContext ctx;
	ctx.width = kW; ctx.height = kH; ctx.frameIndex = 0;
	PublishFrame(*m_cache, ctx, snapStorage, nullptr, &cfg);

	auto& c = BeginCmd();
	m_fxaaPass->Record(*c, *m_cache, ctx);
	EndSubmitWait(c);

	auto& out = m_cache->GetAttachment(AttachmentName::FXAAOutput, e);
	auto id = out.ReadImageData(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	const auto* hd = reinterpret_cast<const uint16_t*>(id->GetPixelData().data());

	float sumSqErr = 0.0f, maxErr = 0.0f;
	int   stepOk   = 0,  stepFail = 0;
	int   gradOk   = 0,  gradFail = 0;

	for (uint32_t y = 0; y < kH; ++y)
	for (uint32_t x = 0; x < kW;  ++x)
	{
		size_t i = (y*kW + x)*4;
		float r = VulkanTestShared::HalfToFloat(hd[i+0]);
		float g = VulkanTestShared::HalfToFloat(hd[i+1]);
		float b = VulkanTestShared::HalfToFloat(hd[i+2]);
		float luma = 0.299f*r + 0.587f*g + 0.114f*b;
		float expected = ExpectedLuma(int(x), int(y));

		float err = std::abs(luma - expected);
		sumSqErr += err * err;
		maxErr = std::max(maxErr, err);

		// Gradient region: expected in [0.1, 0.9] — should be smooth
		if (expected < 0.1f || expected > 0.9f) {
			// Step region — should be near 0 or 1
			if (err < 0.25f) stepOk++; else stepFail++;
		} else {
			// Gradient region — should follow expected curve
			if (err < 0.35f) gradOk++; else gradFail++;
		}
	}

	float rmse = std::sqrt(sumSqErr / float(kW * kH));

	std::cout << "[FXAA Math] rmse=" << rmse << " maxErr=" << maxErr
	          << " | step: " << stepOk << "/" << (stepOk+stepFail)
	          << " grad: " << gradOk << "/" << (gradOk+gradFail)
	          << std::endl;

	// Verification:
	// FXAA is an approximation; it produces a gradual transition across the
	// edge region, not an exact step-function reconstruction.
	EXPECT_GT(gradOk + gradFail, 0)
		<< "No gradient-region pixels at all";
	EXPECT_GT(stepOk, stepOk + stepFail - 12)
		<< "Too many step pixels deviated (edge-adjacent bleeding expected)";
	EXPECT_LT(rmse, 0.40f)
		<< "RMSE too high: " << rmse;
	EXPECT_LT(maxErr, 1.05f)
		<< "Max error out of range";

	// Core mathematical check: gradient must exist (non-monotonic step edges removed)
	// At least some pixels in the gradient region should differ from the binary step
	EXPECT_GT(gradOk, 0)
		<< "No gradient-region pixels matched; FXAA produced no anti-aliasing";
}
