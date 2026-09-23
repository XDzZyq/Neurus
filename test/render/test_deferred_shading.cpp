/**
 * @file test_deferred_shading.cpp
 * @brief Reference-image regression test for the deferred shading pipeline (G-Buffer + HDR passes).
 *
 * Renders a known scene (sphere OBJ + point light) at 256×256 through the
 * full deferred pipeline and captures Position, Normal, Albedo, MetallicRoughness
 * and HDRColor attachments as PNGs.  Each PNG is compared pixel‑wise against
 * a ground‑truth reference image stored in test/render/reference/.
 *
 * On first run the reference images DO NOT exist — the test generates them
 * automatically and reports SKIPPED.  Subsequent runs compare and FAIL on
 * any pixel difference exceeding the allowed tolerance.
 *
 * @note Requires a Vulkan 1.4‑capable GPU.  Skipped in CI without GPU.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"
#include "shared/TestDeferredScene.h"

// --- Render layer ---
#include "render/RenderCache.h"
#include "render/RenderContext.h"
#include "render/UploadManager.h"
#include "render/resources/LightingCache.h"
#include "render/passes/GeometryPass.h"
#include "render/passes/LightingPass.h"
#include "scene/Material.h"
#include "render/Screenshot.h"
#include "render/buffers/IndexBuffer.h"
#include "render/buffers/VertexBuffer.h"
#include "render/Texture.h"

// --- Data layer ---
#include "asset/data/MeshData.h"

// --- Scene layer ---
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include "shared/TestReferenceImage.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace neurus;

// ---------------------------------------------------------------------------
// Attachment list for reference comparison
// ---------------------------------------------------------------------------
static constexpr AttachmentName kReferenceAttachments[] = {
	AttachmentName::Position,
	AttachmentName::Normal,
	AttachmentName::Albedo,
	AttachmentName::MetallicRoughness,
	AttachmentName::HDRColor,
};

static constexpr int kReferenceAttachmentCount = static_cast<int>(std::size(kReferenceAttachments));

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class DeferredShadingTest : public VulkanTestShared
{
protected:
	static constexpr uint32_t kRenderWidth  = 256;
	static constexpr uint32_t kRenderHeight = 256;

	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;

		auto& pd = PhysicalDevice();

		if (pd.getProperties().limits.maxPushConstantsSize < sizeof(LightingPushConstants))
		{
			m_hasVulkan = false;
			return;
		}

		// --- Render pass infrastructure (attachments created lazily) ---
		m_renderCache = std::make_unique<RenderCache>(*m_device, pd);
		m_renderCache->SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily));

		// --- Upload manager for CPU→GPU struct conversion ---

		// --- Geometry pass ---
		m_geometryPass = std::make_unique<GeometryPass>(
			*m_device, pd);

		// --- Lighting pass ---
		m_lightingPass = std::make_unique<LightingPass>(
			*m_device, pd,
			2u);
	}

	void TearDown() override
	{
		VulkanTestShared::TearDown();
	}

	// --- Render pass infrastructure ---
	std::unique_ptr<RenderCache>  m_renderCache;

	std::unique_ptr<GeometryPass>       m_geometryPass;
	std::unique_ptr<LightingPass>       m_lightingPass;
};

// ===========================================================================
// Deferred Shading Reference Image Regression Test
// ===========================================================================

TEST_F(DeferredShadingTest, GbufferAttachments_MatchReferenceImages)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();

 	// -------------------------------------------------------------------
	// Step 1: Load sphere scene (shared helper)
	// -------------------------------------------------------------------
	auto resources = neurus::test::BuildDeferredScene(
		ResolveAssetPath("res/obj/sphere.obj"));

	// -------------------------------------------------------------------
	// Step 7: Transition G-Buffer attachments & build RenderContext
	// -------------------------------------------------------------------
	VulkanTestShared::TransitionGbufferToColorAttachment(*m_renderCache, {kRenderWidth, kRenderHeight}, *this);

	// Pre-register GPU resources before pass recording
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, *resources.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, *resources.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	resources.scene->UseCamera(resources.camera);
	resources.scene->ActivateCamera(resources.camera->GetObjectID());

	RenderContext ctx{
			.width = kRenderWidth, .height = kRenderHeight,
		.frameIndex = 0,
		.editor = { .scene = resources.scene.get() },
	};

	// --- Record geometry pass ---
	{
		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 8: Upload light SSBO & record lighting pass
	// -------------------------------------------------------------------
	{
		Scene testScene;
		testScene.UseLight(resources.light);
		auto lightDict = m_uploadManager->UploadLighting(testScene.light_list);
		m_renderCache->UpdateLighting(lightDict);
	}

	{
		auto& cmd = BeginCmd();
		m_lightingPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 9: Capture attachment screenshots & compare with reference images
	// -------------------------------------------------------------------
	int totalBadPixels = 0;
	bool anyGenerated = false;

	for (int i = 0; i < kReferenceAttachmentCount; ++i)
	{
		const AttachmentName name = kReferenceAttachments[i];
		const std::string refPath = neurus::test::ReferencePath::Make(
			std::string("deferred/") + AttachmentNameToString(name) + ".png");
		const std::string tmpPath = refPath + ".tmp";

		Image& attachment = m_renderCache->GetAttachment(name, {kRenderWidth, kRenderHeight});
		const bool captured = Screenshot::CaptureAttachment(
			*m_device, pd, m_queue, m_graphicsQueueFamily,
			attachment, tmpPath);

		ASSERT_TRUE(captured) << "Failed to capture attachment: "
		                      << AttachmentNameToString(name);

		// Allow a tiny fraction of pixels to drift by a few U8 levels: the
		// G-buffer attachments are bit-stable cross-platform, but HDRColor
		// (post-lighting) picks up ~0.1% edge pixels differing by <=5 levels
		// under MoltenVK/Metal vs native Vulkan. 1% tolerance clears that
		// noise while still catching any real regression.
		const int result = neurus::test::CheckReferenceOrGenerate(refPath, 2, 0.01);
		if (result < 0)
		{
			if (result == -1)
				anyGenerated = true;
			else
				FAIL() << "Failed to load reference image for " << AttachmentNameToString(name);
		}
		else
		{
			totalBadPixels += result;
		}
	}

	if (anyGenerated)
	{
		GTEST_SKIP() << "Reference images generated.  Re-run the test to compare.";
	}
	EXPECT_EQ(totalBadPixels, 0)
		<< totalBadPixels << " pixel(s) differ in reference comparison.";
}
