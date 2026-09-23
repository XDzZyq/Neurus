/**
 * @file test_sun_shadow_intensity.cpp
 * @brief GPU test: uses TestMultiShadow with SUNLIGHT to render a cube-on-plane
 *        scene with multiple shadow-casting sun lights, validates per-light
 *        shadow intensity via staging readback and reference-image regression.
 *
 * Scene (via TestMultiShadow with SUNLIGHT):
 *   - Cube: unit cube [-0.5,0.5]^3 at origin, resting on the plane
 *   - Plane: large quad at z=0 spanning [-10,10] in XY
 *   - 3 sun lights: placed on a ring (z=2, radius=2), direction pointing
 *     toward the cube centre (origin), all shadow-casting
 *   - Camera: at (0, 1, 3) looking at origin, FOV=75°, 256x256
 *
 * Expected shadow pattern per light:
 *   - Pixels behind the cube on the plane: shadowed (intensity > 0)
 *   - Pixels elsewhere on the plane: lit (intensity = 0)
 *   - Background pixels (no geometry): early-out -> lit (0)
 *
 * Reference image regression:
 *   - First run: generates reference PNG -> GTEST_SKIP
 *   - Second run: compares pixel-by-pixel with +-2 tolerance -> PASS
 *
 * @note Requires a Vulkan 1.4-capable GPU. Skipped in CI without GPU.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"
#include "shared/TestMultiShadow.h"

#include "render/shaders/ComputeShader.h"
#include "render/shaders/RenderShader.h"

// --- Render layer ---
#include "render/RenderCache.h"
#include "render/RenderContext.h"
#include "render/passes/ShadowDepthPass.h"
#include "render/passes/ShadowIntensityPass.h"
#include "render/passes/GeometryPass.h"
#include "render/Image.h"
#include "render/Barrier.h"
#include "asset/data/ImageData.h"
#include "core/Log.h"

// --- Scene layer ---
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Scene.h"



#include "shared/TestReferenceImage.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace neurus;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class SunShadowIntensityTest : public VulkanTestShared
{
protected:
	static constexpr uint32_t kRes       = 256;
	static constexpr float    kTolerance = 2.0f / 255.0f;

	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;
		auto& pd = PhysicalDevice();
		m_renderCache = std::make_unique<RenderCache>(*m_device, pd);
		m_renderCache->SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily));
		m_geometryPass = std::make_unique<GeometryPass>(
			*m_device, pd);
		m_shadowDepthPass = std::make_unique<ShadowDepthPass>(
			*m_device, pd, m_queue, m_graphicsQueueFamily,
			ShadowDepthPass::kSunResolution);
		m_shadowIntensityPass = std::make_unique<ShadowIntensityPass>(
			*m_device, pd, 1u);
	}

	void TearDown() override { VulkanTestShared::TearDown(); }

	std::unique_ptr<RenderCache>         m_renderCache;
	std::unique_ptr<GeometryPass>        m_geometryPass;
	std::unique_ptr<ShadowDepthPass>     m_shadowDepthPass;
	std::unique_ptr<ShadowIntensityPass> m_shadowIntensityPass;
};

// ===========================================================================
// SunMultiShadowIntensity — per-light readback + statistical verification
// ===========================================================================

TEST_F(SunShadowIntensityTest, SunMultiShadowIntensity_VerifyNonZero)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto& pd = PhysicalDevice();
	const vk::Extent2D renderExtent(kRes, kRes);

	// -------------------------------------------------------------------
	// Step 1: Load multi-shadow scene with SUNLIGHT (3 lights)
	// -------------------------------------------------------------------
	auto shadowRes = neurus::test::LoadMultiShadow(
		*m_device, pd, m_queue, m_graphicsQueueFamily,
		3, LightType::SUNLIGHT);
	ASSERT_EQ(shadowRes.lightUIDs.size(), 3u) << "Expected 3 sun lights";
	ASSERT_EQ(shadowRes.scene->mesh_list.size(), 2u) << "Expected cube + plane";

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

	// -------------------------------------------------------------------
	// Step 2: Build render context from the scene camera
	// -------------------------------------------------------------------
	ASSERT_FALSE(shadowRes.scene->cam_list.empty()) << "Scene must have a camera";
	const auto& camera = shadowRes.scene->cam_list.begin()->second;
	camera->ChangeCamRatio(static_cast<float>(kRes), static_cast<float>(kRes));

	RenderContext ctx{};
	ctx.width = renderExtent.width; ctx.height = renderExtent.height;
	ctx.frameIndex   = 0;
	ctx.editor.scene = shadowRes.scene.get();

	// -------------------------------------------------------------------
	// Step 3: Run ShadowDepth -> Geometry -> ShadowIntensity (no LightingPass)
	// -------------------------------------------------------------------
	{
		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_shadowDepthPass->Record(*cmd, *m_renderCache, ctx);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		m_shadowIntensityPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 4: Read back R8 shadow intensity for each sun light layer
	// -------------------------------------------------------------------
	auto& intensityArray = m_renderCache->GetShadowIntensityArray(renderExtent);
	const vk::DeviceSize bufSize = static_cast<vk::DeviceSize>(kRes) * kRes;
	const size_t pixelCount = kRes * kRes;

	for (size_t li = 0; li < shadowRes.lightUIDs.size(); ++li)
	{
		int lightUID = shadowRes.lightUIDs[li];
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
			ASSERT_LT(memType, UINT32_MAX) << "No host-visible memory for staging buffer";
			vk::raii::DeviceMemory stagingMem(*m_device, vk::MemoryAllocateInfo(memReqs.size, memType));
			stagingBuf.bindMemory(*stagingMem, 0);

			vk::BufferImageCopy copy{};
			copy.imageSubresource = vk::ImageSubresourceLayers(
				vk::ImageAspectFlagBits::eColor, 0, layer, 1);
			copy.imageExtent = vk::Extent3D(kRes, kRes, 1);
			cmd.copyImageToBuffer(*intensityArray.ImageHandle(),
			                      vk::ImageLayout::eTransferSrcOptimal, *stagingBuf, copy);
			EndSubmitWait(cmd);

			void* mapped = stagingMem.mapMemory(0, bufSize);
			std::memcpy(u8Data.data(), mapped, bufSize);
			stagingMem.unmapMemory();
		}

		// --- Statistical check: both shadowed and lit pixels must exist ---
		size_t shadowedCount = 0, litCount = 0;
		uint8_t maxVal = 0;
		for (size_t p = 0; p < pixelCount; ++p)
		{
			if (u8Data[p] > 0) { ++shadowedCount; if (u8Data[p] > maxVal) maxVal = u8Data[p]; }
			else ++litCount;
		}

		// Verify sun direction for diagnostic output
		const auto& light = shadowRes.scene->light_list.at(lightUID);
		const glm::vec3 sunDir = glm::normalize(light->GetDirection());

		std::cout << "[SunMultiShadowIntensity] Light " << li
		          << " (UID=" << lightUID << ")"
		          << " dir=(" << sunDir.x << "," << sunDir.y << "," << sunDir.z << ")"
		          << " shadowed=" << shadowedCount << " lit=" << litCount
		          << " max=" << static_cast<int>(maxVal)
		          << " total=" << pixelCount << std::endl;

		EXPECT_GT(shadowedCount, 50u)
			<< "Sun light " << li << " (UID=" << lightUID << "): "
			<< "expected at least 50 shadowed pixels — cube should cast a visible shadow on the plane";
		EXPECT_GT(litCount, 100u)
			<< "Sun light " << li << " (UID=" << lightUID << "): "
			<< "expected at least 100 lit pixels — plane pixels outside the shadow should be lit";
	}
}

// ===========================================================================
// SunMultiShadowIntensity_ReferenceImage — reference-image regression
// ===========================================================================

/**
 * @brief Captures per-light sun shadow intensity images and verifies
 *        against stored reference PNGs using first-run-generates /
 *        second-run-compares pattern.
 */
TEST_F(SunShadowIntensityTest, SunMultiShadowIntensity_ReferenceImage)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto& pd = PhysicalDevice();
	const vk::Extent2D renderExtent(kRes, kRes);

	// -------------------------------------------------------------------
	// Step 1: Load scene
	// -------------------------------------------------------------------
	auto shadowRes = neurus::test::LoadMultiShadow(
		*m_device, pd, m_queue, m_graphicsQueueFamily,
		3, LightType::SUNLIGHT);
	ASSERT_EQ(shadowRes.lightUIDs.size(), 3u);

	// Pre-register GPU resources before pass recording
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	// -------------------------------------------------------------------
	// Step 2: Build render context
	// -------------------------------------------------------------------
	ASSERT_FALSE(shadowRes.scene->cam_list.empty());
	const auto& camera = shadowRes.scene->cam_list.begin()->second;
	camera->ChangeCamRatio(static_cast<float>(kRes), static_cast<float>(kRes));

	RenderContext ctx{};
	ctx.width = renderExtent.width; ctx.height = renderExtent.height;
	ctx.frameIndex   = 0;
	ctx.editor.scene = shadowRes.scene.get();

	// -------------------------------------------------------------------
	// Step 3: Record passes
	// -------------------------------------------------------------------
	{
		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_shadowDepthPass->Record(*cmd, *m_renderCache, ctx);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		m_shadowIntensityPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 4: Read back each layer and compare with reference images
	// -------------------------------------------------------------------
	auto& intensityArray = m_renderCache->GetShadowIntensityArray(renderExtent);
	const vk::DeviceSize bufSize = static_cast<vk::DeviceSize>(kRes) * kRes;
	const size_t pixelCount = kRes * kRes;

	bool anyGenerated = false;
	bool allValid = true;

	for (size_t li = 0; li < shadowRes.lightUIDs.size(); ++li)
	{
		int lightUID = shadowRes.lightUIDs[li];
		const uint32_t layer = m_renderCache->GetShadowIntensityLayer(lightUID, renderExtent);
		std::vector<uint8_t> pixelData(pixelCount);

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
			copy.imageSubresource = vk::ImageSubresourceLayers(
				vk::ImageAspectFlagBits::eColor, 0, layer, 1);
			copy.imageExtent = vk::Extent3D(kRes, kRes, 1);
			cmd.copyImageToBuffer(*intensityArray.ImageHandle(),
			                      vk::ImageLayout::eTransferSrcOptimal, *stagingBuf, copy);
			EndSubmitWait(cmd);

			void* mapped = stagingMem.mapMemory(0, bufSize);
			std::memcpy(pixelData.data(), mapped, bufSize);
			stagingMem.unmapMemory();
		}

		// --- Save to temporary PNG ---
		ImageData imgData(pixelData.data(), kRes, kRes, PixelFormat::R8U);
		const std::string refPath = neurus::test::ReferencePath::Make(
			"shadow_intensity/SunIntensity_Light_" + std::to_string(li) + ".png");
		const std::string tmpPath = refPath + ".tmp";
		const bool saved = imgData.SavePNG(tmpPath);
		ASSERT_TRUE(saved) << "Failed to save ShadowIntensity PNG for sun light " << li;

		// --- Reference image regression ---
		const int refResult = neurus::test::CheckReferenceOrGenerate(refPath, 2);

		if (refResult < 0)
		{
			if (refResult == -1)
				anyGenerated = true;
			else
			{
				allValid = false;
				ADD_FAILURE() << "Failed to load reference image for sun light " << li;
			}
		}
		else if (refResult > 0)
		{
			allValid = false;
			ADD_FAILURE() << refResult
				<< " pixel(s) differ from reference for sun light " << li << " (tol=+-2)";
		}

		// --- Content check ---
		size_t nonBlack = 0, nonWhite = 0;
		for (size_t p = 0; p < pixelCount; ++p)
		{
			if (pixelData[p] > 0) ++nonBlack;
			if (pixelData[p] < 255) ++nonWhite;
		}

		std::cout << "[SunMultiShadowIntensityRef] Light " << li
		          << " (UID=" << lightUID << ")"
		          << ": nonBlack=" << nonBlack << " nonWhite=" << nonWhite
		          << " total=" << pixelCount << std::endl;

		ASSERT_GT(nonBlack, 0) << "Shadow intensity for sun light " << li << " is all-black";
		ASSERT_GT(nonWhite, 0) << "Shadow intensity for sun light " << li << " is all-white";
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
