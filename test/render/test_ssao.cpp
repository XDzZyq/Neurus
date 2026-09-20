/**
 * @file test_ssao.cpp
 * @brief Reference-image regression test for the SSAO pass.
 *
 * Renders the Cornell Box scene through geometry pass and SSAO pass at
 * 256×256, then captures the SSAO attachment as a PNG.
 * On first run the reference image does not exist — the test generates it
 * automatically and reports SKIPPED.  Subsequent runs compare pixel-wise
 * and FAIL on any pixel difference exceeding the allowed tolerance.
 *
 * @note Requires a Vulkan 1.4-capable GPU.  Skipped in CI without GPU.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"
#include "shared/TestCornellBox.h"

// --- Render layer ---
#include "render/RenderCache.h"
#include "render/RenderContext.h"
#include "render/passes/GeometryPass.h"
#include "render/passes/SSAOPass.h"
#include "render/Screenshot.h"

#include "shared/TestReferenceImage.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace neurus;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class SSAOTest : public VulkanTestShared
{
protected:
	static constexpr uint32_t kRenderWidth  = 256;
	static constexpr uint32_t kRenderHeight = 256;

	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;

		auto& pd = PhysicalDevice();

		// --- Render pass infrastructure (attachments created lazily) ---
		m_renderCache = std::make_unique<RenderCache>(*m_device, pd);
		m_renderCache->SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily));

		// --- Geometry pass ---
		m_geometryPass = std::make_unique<GeometryPass>(
			*m_device, pd);

		// --- SSAO pass ---
		m_ssaoPass = std::make_unique<SSAOPass>(
			*m_device, pd,
			1u,   // one descriptor set for single-frame test
			m_queue, m_graphicsQueueFamily);
	}

	void TearDown() override
	{
		VulkanTestShared::TearDown();
	}

	// --- Render pass infrastructure ---
	std::unique_ptr<RenderCache>  m_renderCache;
	std::unique_ptr<GeometryPass>       m_geometryPass;
	std::unique_ptr<SSAOPass>           m_ssaoPass;
};

// ===========================================================================
// SSAO Reference Image Regression Test
// ===========================================================================

TEST_F(SSAOTest, SSAOAttachment_MatchesReferenceImage)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();

	// -------------------------------------------------------------------
	// -------------------------------------------------------------------
	// Step 1: Load Cornell Box scene
	// -------------------------------------------------------------------
	auto cb = test::LoadCornellBox(*m_device, pd, m_queue, m_graphicsQueueFamily);
	ASSERT_GT(cb.scene->mesh_list.size(), 0u) << "No meshes loaded for Cornell Box";

	// Adjust camera aspect ratio to match the render target
	cb.camera->ChangeCamRatio(
		static_cast<float>(kRenderWidth),
		static_cast<float>(kRenderHeight));

	// -------------------------------------------------------------------
	// Step 2: Transition G-Buffer & build RenderContext
	// -------------------------------------------------------------------
	VulkanTestShared::TransitionGbufferToColorAttachment(*m_renderCache, {kRenderWidth, kRenderHeight}, *this);

	// Pre-register mesh GPU resources before pass recording
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, *cb.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	cb.scene->UseCamera(cb.camera);

	RenderContext ctx{
			.width = kRenderWidth, .height = kRenderHeight,
		.editor = { .scene = cb.scene.get() },
	};

	// --- Record geometry pass ---
	{
		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 3: Run SSAO pass
	// -------------------------------------------------------------------
	{
		auto& cmd = BeginCmd();
		m_ssaoPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 4: Capture SSAO attachment & compare with reference
	// -------------------------------------------------------------------
	const std::string refPath = neurus::test::ReferencePath::Make("ssao/SSAO.png");
	const std::string tmpPath = refPath + ".tmp";

	Image& ssaoAttachment = m_renderCache->GetAttachment(AttachmentName::SSAO, {kRenderWidth, kRenderHeight});
	const bool captured = Screenshot::CaptureAttachment(
		*m_device, pd, m_queue, m_graphicsQueueFamily,
		ssaoAttachment, tmpPath);

	ASSERT_TRUE(captured) << "Failed to capture SSAO attachment";

	// SSAO is under-sampled (16 hemisphere samples) and its per-sample
	// occlusion test hinges on a hard `delta <= 0` branch fed by a dependent
	// texture fetch at a projected UV (see ssao.comp). Sub-ULP float
	// differences between MoltenVK/Metal and native Vulkan flip individual
	// samples across that threshold, so ~1-4% of pixels — concentrated on AO
	// edges — differ by a few U8 levels across platforms. The CPU RNG is a
	// deterministic xorshift32 (identical on all platforms), so this is pure
	// GPU float divergence, not a seed mismatch. Allow up to 2% of pixels to
	// exceed 32/channel; a real regression shifts far more of the image.
	const int result = neurus::test::CheckReferenceOrGenerate(refPath, 32, 0.02);
	if (result < 0)
		GTEST_SKIP() << "Reference image generated. Re-run the test to compare.";
	EXPECT_EQ(result, 0) << result
		<< " pixel(s) exceed the SSAO tolerance (32/channel, >2% of pixels).";
}
