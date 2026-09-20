/**
 * @file test_multilight_shadow.cpp
 * @brief TDD reference-image regression test for multi-light shadow rendering.
 *
 * Renders a cube-on-plane scene with 2 shadow-casting point lights through
 * the full deferred pipeline (ShadowDepthPass → GeometryPass → ShadowIntensityPass
 * → LightingPass) and validates HDRColor output via reference image regression.
 *
 * Reference image pattern:
 *   - First run: generates reference PNG → GTEST_SKIP
 *   - Second run: compares pixel-by-pixel with ±2 tolerance → PASS/FAIL
 *
 * This is the RED phase of TDD — the test compiles and runs but the shadow
 * passes initially only handle a single light.  The reference generated on first run will reflect
 * single-shadow behaviour.  When shadow pass loops are implemented
 * (Wave 2-3), the reference must be regenerated.
 *
 * @note Requires a Vulkan 1.4-capable GPU.  Skipped in CI without GPU.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"
#include "shared/TestMultiShadow.h"

#include "render/shaders/RenderShader.h"

// --- Render layer ---
#include "render/RenderCache.h"
#include "render/RenderContext.h"
#include "render/UploadManager.h"
#include "render/resources/LightingCache.h"
#include "render/passes/GeometryPass.h"
#include "render/passes/ShadowDepthPass.h"
#include "render/passes/ShadowIntensityPass.h"
#include "render/passes/LightingPass.h"
#include "render/Screenshot.h"

// --- Scene layer ---
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Scene.h"



#include "shared/TestReferenceImage.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace neurus;

// ===========================================================================
// Test fixture
// ===========================================================================

class MultiLightShadowTest : public VulkanTestShared
{
protected:
	static constexpr uint32_t kRenderWidth  = 256;
	static constexpr uint32_t kRenderHeight = 256;

	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;

		auto& pd = PhysicalDevice();

		// --- Push constant budget check ---
		if (pd.getProperties().limits.maxPushConstantsSize < sizeof(LightingPushConstants))
		{
			m_hasVulkan = false;
			return;
		}

		// --- Render pass infrastructure (attachments created lazily) ---
		m_renderCache = std::make_unique<RenderCache>(*m_device, pd);
		m_renderCache->SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily));

		// --- Geometry pass ---
		m_geometryPass = std::make_unique<GeometryPass>(
			*m_device, pd);

		// --- Shadow depth pass ---
		m_shadowDepthPass = std::make_unique<ShadowDepthPass>(
			*m_device, pd, m_queue, m_graphicsQueueFamily,
			ShadowDepthPass::kDefaultResolution);
		// --- Shadow intensity pass (1 set = single-frame recording) ---
		m_shadowIntensityPass = std::make_unique<ShadowIntensityPass>(
			*m_device, pd, 1u);

		// --- Lighting pass (2 sets = matches test_deferred_shading) ---
		m_lightingPass = std::make_unique<LightingPass>(
			*m_device, pd, 2u);

		// --- Upload manager for CPU→GPU struct conversion ---
	}

	void TearDown() override
	{
		VulkanTestShared::TearDown();
	}

	std::unique_ptr<RenderCache>         m_renderCache;

	std::unique_ptr<GeometryPass>        m_geometryPass;
	std::unique_ptr<ShadowDepthPass>     m_shadowDepthPass;
	std::unique_ptr<ShadowIntensityPass> m_shadowIntensityPass;
	std::unique_ptr<LightingPass>        m_lightingPass;
};

// ===========================================================================
// TwoShadowLights_HDRColorReference — reference image regression
// ===========================================================================

TEST_F(MultiLightShadowTest, TwoShadowLights_HDRColorReference)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto& pd = PhysicalDevice();
	const vk::Extent2D renderExtent(kRenderWidth, kRenderHeight);

	// -------------------------------------------------------------------
	// Step 1: Load multi-shadow scene (2 lights by default)
	// -------------------------------------------------------------------
	auto shadowRes = neurus::test::LoadMultiShadow(
		*m_device, pd, m_queue, m_graphicsQueueFamily);
	ASSERT_EQ(shadowRes.scene->mesh_list.size(), 2u) << "Expected cube + plane (2 meshes)";
	ASSERT_EQ(shadowRes.lightUIDs.size(), 3u) << "Expected 3 shadow-casting lights";

	// Pre-register GPU resources before pass recording
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	// Get the camera from the scene
	ASSERT_FALSE(shadowRes.scene->cam_list.empty()) << "Scene must have a camera";
	const auto& camera = shadowRes.scene->cam_list.begin()->second;

	// Update camera aspect ratio to match render extent
	camera->ChangeCamRatio(static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight));
	const glm::vec3 cameraPos = camera->GetPosition();

	NEURUS_LOG("[MultiLightShadow] Camera@("
	           << cameraPos.x << "," << cameraPos.y << "," << cameraPos.z << ")"
	           << " lights=" << shadowRes.lightUIDs.size());

	// -------------------------------------------------------------------
	// Step 2: Build RenderContext with both light UIDs
	// -------------------------------------------------------------------
	RenderContext ctx{};
	ctx.width = renderExtent.width; ctx.height = renderExtent.height;
	ctx.frameIndex   = 0;
	// renderItems removed — ShadowDepthPass iterates scene.mesh_list directly
	ctx.editor.scene = shadowRes.scene.get();

	// -------------------------------------------------------------------
	// Step 3: Transition G-Buffer to renderable layouts
	// -------------------------------------------------------------------
	VulkanTestShared::TransitionGbufferToColorAttachment(
		*m_renderCache, renderExtent, *this);

	// -------------------------------------------------------------------
	// Step 4: Upload lights to RenderCache via UploadManager
	// -------------------------------------------------------------------
	{
		auto lightDict = m_uploadManager->UploadLighting(shadowRes.scene->light_list);
		m_renderCache->UpdateLighting(lightDict);
	}

	// -------------------------------------------------------------------
	// Step 5: Record all passes in a single command buffer
	//         Order: ShadowDepth (independent) → Geometry (writes G-Buffer)
	//                → ShadowIntensity (reads Position + cubemap)
	//                → Lighting (reads G-Buffer + shadow intensity)
	// -------------------------------------------------------------------
	{
		auto& cmd = BeginCmd();
		m_shadowDepthPass->Record(*cmd, *m_renderCache, ctx);
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		m_shadowIntensityPass->Record(*cmd, *m_renderCache, ctx);
		m_lightingPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 6: Capture HDRColor attachment as PNG
	// -------------------------------------------------------------------
	const std::string refPath =
		neurus::test::ReferencePath::Make("multilight/TwoLights_HDRColor.png");
	const std::string tmpPath = refPath + ".tmp";

	Image& hdrAttachment = m_renderCache->GetAttachment(
		AttachmentName::HDRColor, renderExtent);

	const bool captured = Screenshot::CaptureAttachment(
		*m_device, pd, m_queue, m_graphicsQueueFamily,
		hdrAttachment, tmpPath);

	ASSERT_TRUE(captured) << "Failed to capture HDRColor attachment to " << tmpPath;

	// -------------------------------------------------------------------
	// Step 7: Reference image regression
	// Tolerance widened for platform-specific float/texture-sampling
	// differences (GPU vendor/driver, MoltenVK vs. native Vulkan) around
	// shadow edges: +/-4 per channel, up to 1% of pixels may differ.
	// -------------------------------------------------------------------
	const int refResult = neurus::test::CheckReferenceOrGenerate(refPath, 4, 0.01);

	if (refResult < 0)
	{
		// First run — reference generated (return -1) or load failure (-2)
		if (refResult == -1)
			GTEST_SKIP() << "Reference images generated.  Re-run the test to compare.";
		else
			FAIL() << "Failed to load reference image for HDRColor";
	}
	else
	{
		// Second run — compare against reference
		EXPECT_EQ(refResult, 0) << refResult << " pixel(s) differ from reference (tol=±2)";
	}
}

// ===========================================================================
// TwoLights_NoVUID — smoke test (pipeline runs without crash)
// ===========================================================================

TEST_F(MultiLightShadowTest, TwoLights_NoVUID)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto& pd = PhysicalDevice();
	const vk::Extent2D renderExtent(kRenderWidth, kRenderHeight);

	// -------------------------------------------------------------------
	// Load scene
	// -------------------------------------------------------------------
	auto shadowRes = neurus::test::LoadMultiShadow(
		*m_device, pd, m_queue, m_graphicsQueueFamily);
	ASSERT_FALSE(shadowRes.scene->cam_list.empty());

	const auto& camera = shadowRes.scene->cam_list.begin()->second;
	camera->ChangeCamRatio(static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight));

	// -------------------------------------------------------------------
	// Build context
	// -------------------------------------------------------------------
	RenderContext ctx{};
	ctx.width = renderExtent.width; ctx.height = renderExtent.height;
	// renderItems removed — ShadowDepthPass iterates scene.mesh_list directly
	ctx.editor.scene = shadowRes.scene.get();

	// -------------------------------------------------------------------
	// Transition + upload + record
	// -------------------------------------------------------------------
	VulkanTestShared::TransitionGbufferToColorAttachment(
		*m_renderCache, renderExtent, *this);

	{
		auto lightDict = m_uploadManager->UploadLighting(shadowRes.scene->light_list);
		m_renderCache->UpdateLighting(lightDict);
	}

	{
		auto& cmd = BeginCmd();
		m_shadowDepthPass->Record(*cmd, *m_renderCache, ctx);
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		m_shadowIntensityPass->Record(*cmd, *m_renderCache, ctx);
		m_lightingPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// If we reach here without crash/exception, the test passes.
	// Validation layer errors are printed to stderr by the debug callback
	// and will appear in ctest --output-on-failure output.
	SUCCEED();
}

// ===========================================================================
// ShadowIntensityReadback — verify per-light shadow intensity data
// ===========================================================================

/**
 * @brief Reads back every ShadowIntensity image and verifies non-trivial data.
 *
 * Uses the same manual staging-buffer readback pattern established in
 * test_shadow_intensity.cpp.  Validates that shadow intensity images
 * contain both shadowed (==1.0) and lit (==0.0) pixels — an all-zero
 * or all-one result indicates a broken shadow evaluation path.
 */
TEST_F(MultiLightShadowTest, ShadowIntensityReadback_VerifyNonZero)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto& pd = PhysicalDevice();
	const vk::Extent2D renderExtent(kRenderWidth, kRenderHeight);
	// -------------------------------------------------------------------
	// Load scene
	// -------------------------------------------------------------------
	auto shadowRes = neurus::test::LoadMultiShadow(
		*m_device, pd, m_queue, m_graphicsQueueFamily, 3 /*numLights*/);
	ASSERT_FALSE(shadowRes.scene->cam_list.empty());
	ASSERT_EQ(shadowRes.lightUIDs.size(), 3u) << "Expected 3 shadow-casting lights";

	// Pre-register GPU resources before pass recording
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	const auto& camera = shadowRes.scene->cam_list.begin()->second;
	camera->ChangeCamRatio(static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight));

	// -------------------------------------------------------------------
	// Build context
	// -------------------------------------------------------------------
	RenderContext ctx{};
	ctx.width = renderExtent.width; ctx.height = renderExtent.height;
	// renderItems removed — ShadowDepthPass iterates scene.mesh_list directly
	ctx.editor.scene = shadowRes.scene.get();

	// -------------------------------------------------------------------
	// Transition G-Buffer
	// -------------------------------------------------------------------
	VulkanTestShared::TransitionGbufferToColorAttachment(
		*m_renderCache, renderExtent, *this);

	// -------------------------------------------------------------------
	// Run ONLY ShadowDepth → Geometry → ShadowIntensity (NO LightingPass)
	// to isolate whether LightingPass corrupts shadow intensity data.
	// -------------------------------------------------------------------
	{
		auto& cmd = BeginCmd();
		m_shadowDepthPass->Record(*cmd, *m_renderCache, ctx);
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		m_shadowIntensityPass->Record(*cmd, *m_renderCache, ctx);
		// NO LightingPass here — pure isolation test
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Read back every shadow intensity image from the layered array
	// -------------------------------------------------------------------
	const vk::DeviceSize bufSize = static_cast<vk::DeviceSize>(kRenderWidth) * kRenderHeight;
	const size_t pixelCount = kRenderWidth * kRenderHeight;

	auto& intensityArray = m_renderCache->GetShadowIntensityArray(renderExtent);

	for (int lightUID : shadowRes.lightUIDs)
	{
		const uint32_t layer = m_renderCache->GetShadowIntensityLayer(lightUID, renderExtent);
		std::vector<uint8_t> u8Data(pixelCount);

		{
			auto& cmd = BeginCmd();
			Barrier::Transition(*cmd, intensityArray, ImageState::TransferSrc);

			vk::raii::Buffer stagingBuf(*m_device,
				vk::BufferCreateInfo({}, bufSize, vk::BufferUsageFlagBits::eTransferDst));
			auto memReqs = stagingBuf.getMemoryRequirements();
			uint32_t memType = VulkanTestShared::FindMemoryType(pd, memReqs.memoryTypeBits,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
			ASSERT_LT(memType, UINT32_MAX) << "No host-visible memory type for staging buffer";
			vk::raii::DeviceMemory stagingMem(*m_device, vk::MemoryAllocateInfo(memReqs.size, memType));
			stagingBuf.bindMemory(*stagingMem, 0);

			vk::BufferImageCopy copy{};
			copy.imageSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, layer, 1);
			copy.imageExtent = vk::Extent3D(kRenderWidth, kRenderHeight, 1);
			cmd.copyImageToBuffer(*intensityArray.ImageHandle(),
			                      vk::ImageLayout::eTransferSrcOptimal, *stagingBuf, copy);
			EndSubmitWait(cmd);

			void* mapped = stagingMem.mapMemory(0, bufSize);
			std::memcpy(u8Data.data(), mapped, bufSize);
			stagingMem.unmapMemory();
		}

		// --- Count non-zero (shadowed) and zero (lit) pixels ---
		int shadowed = 0, lit = 0;
		uint8_t maxVal = 0;
		for (size_t i = 0; i < pixelCount; ++i)
		{
			if (u8Data[i] > 0) { ++shadowed; if (u8Data[i] > maxVal) maxVal = u8Data[i]; }
			else ++lit;
		}

		std::cout << "[ShadowIntensityReadback] Light " << lightUID
		          << ": shadowed=" << shadowed << " lit=" << lit
		          << " max=" << static_cast<int>(maxVal)
		          << " total=" << pixelCount << std::endl;

		// --- Assertions ---
		// At minimum we expect some pixels to be shadowed (the cube casts shadow
		// on the plane) and some to be lit (the plane outside the shadow).
		// The exact counts depend on geometry but zero shadowed pixels is always wrong.
		EXPECT_GT(shadowed, 0)
			<< "Light " << lightUID << " shadow intensity is ALL ZERO — "
			<< "shadow evaluation pass may be broken (G-Buffer Position, cubemap depth, or dispatch)";
		EXPECT_GT(lit, 0)
			<< "Light " << lightUID << " shadow intensity is ALL ONE — "
			<< "unlikely, check bias/farPlane parameters";
	}

	SUCCEED();
}

// ===========================================================================
// ShadowIntensityPerLight_ReferenceImage — per-light shadow intensity regression
// ===========================================================================

/**
 * @brief Captures per-light ShadowIntensity images as PNGs and verifies
 *        they are not all-black nor all-white.
 *
 * Runs the full deferred+PBR pipeline (ShadowDepth → Geometry →
 * ShadowIntensity → Lighting) with 2 lights, then reads back each
 * light's ShadowIntensity (R8_UNORM) attachment through a staging
 * buffer.  Saves the captured data as a reference PNG and uses the
 * first-run-generates / second-run-compares pattern.  Additionally
 * asserts that each intensity map contains both lit (0) and shadowed
 * (>0) pixels.
 */
TEST_F(MultiLightShadowTest, ShadowIntensityPerLight_ReferenceImage)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto& pd = PhysicalDevice();
	const vk::Extent2D renderExtent(kRenderWidth, kRenderHeight);

	// -------------------------------------------------------------------
	// Load scene (2 lights by default)
	// -------------------------------------------------------------------
	auto shadowRes = neurus::test::LoadMultiShadow(
		*m_device, pd, m_queue, m_graphicsQueueFamily);
	ASSERT_EQ(shadowRes.lightUIDs.size(), 3u) << "Expected 3 shadow-casting lights";

	// Pre-register GPU resources before pass recording
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	const auto& camera = shadowRes.scene->cam_list.begin()->second;
	camera->ChangeCamRatio(static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight));

	// -------------------------------------------------------------------
	// Build context
	// -------------------------------------------------------------------
	RenderContext ctx{};
	ctx.width = renderExtent.width; ctx.height = renderExtent.height;
	// renderItems removed — ShadowDepthPass iterates scene.mesh_list directly
	ctx.editor.scene = shadowRes.scene.get();

	// -------------------------------------------------------------------
	// Transition G-Buffer
	// -------------------------------------------------------------------
	VulkanTestShared::TransitionGbufferToColorAttachment(
		*m_renderCache, renderExtent, *this);

	// -------------------------------------------------------------------
	// Upload lights to RenderCache via UploadManager
	// -------------------------------------------------------------------
	{
		auto lightDict = m_uploadManager->UploadLighting(shadowRes.scene->light_list);
		m_renderCache->UpdateLighting(lightDict);
	}

	// -------------------------------------------------------------------
	// Run full pipeline (ShadowDepth → Geometry → ShadowIntensity → Lighting)
	// -------------------------------------------------------------------
	{
		auto& cmd = BeginCmd();
		m_shadowDepthPass->Record(*cmd, *m_renderCache, ctx);
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		m_shadowIntensityPass->Record(*cmd, *m_renderCache, ctx);
		m_lightingPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Read back each shadow intensity layer from the layered array
	// -------------------------------------------------------------------
	const vk::DeviceSize bufSize = static_cast<vk::DeviceSize>(kRenderWidth) * kRenderHeight;
	const size_t pixelCount = kRenderWidth * kRenderHeight;

	auto& intensityArray = m_renderCache->GetShadowIntensityArray(renderExtent);

	bool anyGenerated = false;
	bool allValid = true;

	for (size_t li = 0; li < shadowRes.lightUIDs.size(); ++li)
	{
		int lightUID = shadowRes.lightUIDs[li];
		const uint32_t layer = m_renderCache->GetShadowIntensityLayer(lightUID, renderExtent);
		std::vector<uint8_t> pixelData(pixelCount);

		// --- Manual staging-buffer readback ---
		{
			auto& cmd = BeginCmd();
			Barrier::Transition(*cmd, intensityArray, ImageState::TransferSrc);

			vk::raii::Buffer stagingBuf(*m_device,
				vk::BufferCreateInfo({}, bufSize, vk::BufferUsageFlagBits::eTransferDst));
			auto memReqs = stagingBuf.getMemoryRequirements();
			uint32_t memType = VulkanTestShared::FindMemoryType(pd, memReqs.memoryTypeBits,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
			ASSERT_LT(memType, UINT32_MAX) << "No host-visible memory for staging buffer";
			vk::raii::DeviceMemory stagingMem(*m_device, vk::MemoryAllocateInfo(memReqs.size, memType));
			stagingBuf.bindMemory(*stagingMem, 0);

			vk::BufferImageCopy copy{};
			copy.imageSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, layer, 1);
			copy.imageExtent = vk::Extent3D(kRenderWidth, kRenderHeight, 1);
			cmd.copyImageToBuffer(*intensityArray.ImageHandle(),
			                      vk::ImageLayout::eTransferSrcOptimal, *stagingBuf, copy);
			EndSubmitWait(cmd);

			void* mapped = stagingMem.mapMemory(0, bufSize);
			std::memcpy(pixelData.data(), mapped, bufSize);
			stagingMem.unmapMemory();
		}

		// --- Save to temporary PNG ---
		ImageData imgData(pixelData.data(), kRenderWidth, kRenderHeight, PixelFormat::R8U);
		const std::string refPath = neurus::test::ReferencePath::Make(
			"multilight/ShadowIntensity_Light_" + std::to_string(li) + ".png");
		const std::string tmpPath = refPath + ".tmp";
		const bool saved = imgData.SavePNG(tmpPath);
		ASSERT_TRUE(saved) << "Failed to save ShadowIntensity PNG for light " << li;

		// --- Reference image regression (widened tolerance: see note above) ---
		const int refResult = neurus::test::CheckReferenceOrGenerate(refPath, 4, 0.01);

		if (refResult < 0)
		{
			// First run — reference generated
			if (refResult == -1)
				anyGenerated = true;
			else
			{
				allValid = false;
				ADD_FAILURE() << "Failed to load reference image for light " << li;
			}
		}
		else if (refResult > 0)
		{
			allValid = false;
			ADD_FAILURE() << refResult
				<< " pixel(s) differ from reference for light " << li << " (tol=±4, 1% budget)";
		}

		// --- Verify content is not all-black nor all-white ---
		size_t nonBlack = 0, nonWhite = 0;
		for (size_t p = 0; p < pixelCount; ++p)
		{
			if (pixelData[p] > 0) ++nonBlack;
			if (pixelData[p] < 255) ++nonWhite;
		}

		std::cout << "[ShadowIntensityPerLight] Light " << li
		          << " (UID=" << lightUID << ")"
		          << ": nonBlack=" << nonBlack << " nonWhite=" << nonWhite
		          << " total=" << pixelCount << std::endl;

		ASSERT_GT(nonBlack, 0)
			<< "Shadow intensity for light " << li << " is all-black";
		ASSERT_GT(nonWhite, 0)
			<< "Shadow intensity for light " << li << " is all-white";
	}

	if (anyGenerated)
	{
		GTEST_SKIP() << "Reference images generated.  Re-run the test to compare.";
	}

	if (!allValid)
	{
		FAIL() << "One or more reference image comparisons failed.  See details above.";
	}
}

// ===========================================================================
// SunLights_HDRColorReference — full deferred pipeline with sun shadows
// ===========================================================================

/**
 * @brief Renders a cube-on-plane scene with 3 sun lights through the full
 *        deferred pipeline and validates HDRColor output via reference image
 *        regression at "deferred/SunMultiShadow_HDRColor.png".
 *
 * Uses TestMultiShadow with LightType::SUNLIGHT to generate a ring of
 * directional sun lights pointing toward the cube centre.  The full pipeline
 * (ShadowDepth → Geometry → ShadowIntensity → Lighting) runs and the final
 * HDRColor attachment is captured for regression testing.
 */
TEST_F(MultiLightShadowTest, SunLights_HDRColorReference)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto& pd = PhysicalDevice();
	const vk::Extent2D renderExtent(kRenderWidth, kRenderHeight);

	// -------------------------------------------------------------------
	// Step 1: Load multi-shadow scene with SUNLIGHT (3 lights)
	// -------------------------------------------------------------------
	auto shadowRes = neurus::test::LoadMultiShadow(
		*m_device, pd, m_queue, m_graphicsQueueFamily,
		3, LightType::SUNLIGHT);
	ASSERT_EQ(shadowRes.scene->mesh_list.size(), 2u) << "Expected cube + plane (2 meshes)";
	ASSERT_EQ(shadowRes.lightUIDs.size(), 3u) << "Expected 3 sun lights";

	// Pre-register GPU resources before pass recording
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	// Verify all lights are SUNLIGHT
	for (int uid : shadowRes.lightUIDs)
	{
		const auto& light = shadowRes.scene->light_list.at(uid);
		ASSERT_EQ(light->light_type, LightType::SUNLIGHT)
			<< "Light " << uid << " should be SUNLIGHT";
		ASSERT_TRUE(light->use_shadow)
			<< "Light " << uid << " should cast shadows";
	}

	// Get the camera from the scene
	ASSERT_FALSE(shadowRes.scene->cam_list.empty()) << "Scene must have a camera";
	const auto& camera = shadowRes.scene->cam_list.begin()->second;

	// Update camera aspect ratio to match render extent
	camera->ChangeCamRatio(static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight));
	const glm::vec3 cameraPos = camera->GetPosition();

	NEURUS_LOG("[SunMultiShadowHDR] Camera@("
	           << cameraPos.x << "," << cameraPos.y << "," << cameraPos.z << ")"
	           << " sunLights=" << shadowRes.lightUIDs.size());

	// -------------------------------------------------------------------
	// Step 2: Build RenderContext
	// -------------------------------------------------------------------
	RenderContext ctx{};
	ctx.width = renderExtent.width; ctx.height = renderExtent.height;
	ctx.frameIndex   = 0;
	// renderItems removed — ShadowDepthPass iterates scene.mesh_list directly
	ctx.editor.scene = shadowRes.scene.get();

	// -------------------------------------------------------------------
	// Step 3: Transition G-Buffer to renderable layouts
	// -------------------------------------------------------------------
	VulkanTestShared::TransitionGbufferToColorAttachment(
		*m_renderCache, renderExtent, *this);

	// -------------------------------------------------------------------
	// Step 4: Build shadow index map (SUNLIGHT) and upload lights
	// -------------------------------------------------------------------
	{
		std::vector<int32_t> shadowUIDs;
		for (const auto& [id, light] : shadowRes.scene->light_list)
		{
			if (light && light->light_type == LightType::SUNLIGHT && light->use_shadow)
				shadowUIDs.push_back(id);
		}
		std::sort(shadowUIDs.begin(), shadowUIDs.end());

		std::unordered_map<int32_t, int> shadowIndexMap;
		for (size_t i = 0; i < shadowUIDs.size(); ++i)
			shadowIndexMap[shadowUIDs[i]] = static_cast<int>(i);

		auto lightDict = m_uploadManager->UploadLighting(shadowRes.scene->light_list);
		m_renderCache->UpdateLighting(lightDict);
	}

	// -------------------------------------------------------------------
	// Step 5: Record all passes in a single command buffer
	// -------------------------------------------------------------------
	{
		auto& cmd = BeginCmd();
		m_shadowDepthPass->Record(*cmd, *m_renderCache, ctx);
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		m_shadowIntensityPass->Record(*cmd, *m_renderCache, ctx);
		m_lightingPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 6: Capture HDRColor attachment as PNG
	// -------------------------------------------------------------------
	const std::string refPath =
		neurus::test::ReferencePath::Make("deferred/SunMultiShadow_HDRColor.png");
	const std::string tmpPath = refPath + ".tmp";

	Image& hdrAttachment = m_renderCache->GetAttachment(
		AttachmentName::HDRColor, renderExtent);

	const bool captured = Screenshot::CaptureAttachment(
		*m_device, pd, m_queue, m_graphicsQueueFamily,
		hdrAttachment, tmpPath);

	ASSERT_TRUE(captured) << "Failed to capture HDRColor attachment to " << tmpPath;

	// -------------------------------------------------------------------
	// Step 7: Reference image regression (widened tolerance: see note above)
	// -------------------------------------------------------------------
	const int refResult = neurus::test::CheckReferenceOrGenerate(refPath, 4, 0.01);

	if (refResult < 0)
	{
		if (refResult == -1)
			GTEST_SKIP() << "Reference image generated.  Re-run the test to compare.";
		else
			FAIL() << "Failed to load reference image for SunMultiShadow_HDRColor";
	}
	else
	{
		EXPECT_EQ(refResult, 0) << refResult << " pixel(s) differ from reference (tol=±4, 1% budget)";
	}
}
