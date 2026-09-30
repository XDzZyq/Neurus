/**
 * @file test_shadow_intensity.cpp
 * @brief GPU test: renders cube+plane scene through GeometryPass → ShadowDepthPass →
 *        ShadowIntensityPass, reads back R8 shadow intensity data, and verifies
 *        against mathematically-computed expected shadow values.
 *
 * Mathematical verification:
 *   - Cube at [-0.5, 0.5] x [-0.5, 0.5] x [2.5, 3.5] (centre at (0,0,3))
 *   - Plane at z=0, spanning [-10, 10] in XY
 *   - Point light at (0, 0, 6), farPlane = Light::point_shadow_far = 10.0
 *   - Camera at (0, 0.001, 2) looking at (0, 0, 0), FOV=75°, 256×256
 *   - Unproject each pixel to world-space; intersect with plane; ray-AABB
 *     test to determine shadow = 1.0 (occluded) or 0.0 (lit).
 *   - Tolerance: ±2/255 for binary shadow values.
 *
 * Reference image regression:
 *   - First run: generates reference PNG → GTEST_SKIP
 *   - Second run: compares pixel-by-pixel with ±2 tolerance → PASS
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"
#include "shared/TestSimpleShadow.h"

#include "render/shaders/ComputeShader.h"
#include "render/shaders/RenderShader.h"

#include "render/passes/ShadowDepthPass.h"
#include "render/passes/ShadowIntensityPass.h"
#include "render/passes/GeometryPass.h"
#include "render/RenderContext.h"
#include "render/Image.h"
#include "render/Barrier.h"
#include "render/RenderCache.h"
#include "asset/data/ImageData.h"
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
#include <vector>

using namespace neurus;

class ShadowIntensityTest : public VulkanTestShared
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
			ShadowDepthPass::kDefaultResolution);
		m_shadowIntensityPass = std::make_unique<ShadowIntensityPass>(
			*m_device, pd, 1u);
	}
	void TearDown() override { VulkanTestShared::TearDown(); }

	static float ComputeExpectedShadow(uint32_t px, uint32_t py,
		const glm::mat4& invProjView, const glm::vec3& cameraPos,
		const glm::vec3& lightPos, const glm::vec3& cubeAABBLo,
		const glm::vec3& cubeAABBHi)
	{
		const float ndcX = 2.0f * (px + 0.5f) / kRes - 1.0f;
		const float ndcY = 2.0f * (py + 0.5f) / kRes - 1.0f;
		const glm::vec4 wn4 = invProjView * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
		const glm::vec3 wn = glm::vec3(wn4) / wn4.w;
		const glm::vec3 rd = glm::normalize(wn - cameraPos);

		const float tP = -cameraPos.z / rd.z;
		if (tP <= 0.0f) return 0.0f;
		const glm::vec3 pHit = cameraPos + tP * rd;
		if (std::abs(pHit.x) > 10.0f || std::abs(pHit.y) > 10.0f) return 0.0f;

		const glm::vec3 tf = pHit - lightPos;
		const float df = glm::length(tf);
		const glm::vec3 ld = tf / df;
		constexpr float eps = 0.001f;
		float tMin = eps, tMax = df;
		for (int a = 0; a < 3; ++a)
		{
			const float o = (&lightPos.x)[a], d = (&ld.x)[a];
			const float lo = (&cubeAABBLo.x)[a], hi = (&cubeAABBHi.x)[a];
			if (std::abs(d) > 1e-7f)
			{
				const float t1 = (lo - o) / d, t2 = (hi - o) / d;
				tMin = std::max(tMin, std::min(t1, t2));
				tMax = std::min(tMax, std::max(t1, t2));
			}
			else if (o < lo || o > hi) return 0.0f;
		}
		return (tMin < tMax) ? 1.0f : 0.0f;
	}

	std::unique_ptr<RenderCache>         m_renderCache;
	std::unique_ptr<GeometryPass>        m_geometryPass;
	std::unique_ptr<ShadowDepthPass>     m_shadowDepthPass;
	std::unique_ptr<ShadowIntensityPass> m_shadowIntensityPass;
};

TEST_F(ShadowIntensityTest, ShadowIntensity_MatchesExpectedAndReference)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto& pd = PhysicalDevice();
	const vk::Extent2D renderExtent(kRes, kRes);

	// -------------------------------------------------------------------
	// Step 1: Load test scene
	// -------------------------------------------------------------------
	auto shadowRes = neurus::test::LoadSimpleShadow(
		*m_device, pd, m_queue, m_graphicsQueueFamily);
	ASSERT_EQ(shadowRes.scene->mesh_list.size(), 2u);

	// Pre-register GPU resources before pass recording
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	const auto& camera   = shadowRes.scene->cam_list.begin()->second;
	const int   lightUID = shadowRes.scene->light_list.begin()->first;
	const auto& light    = shadowRes.scene->light_list.begin()->second;

	const glm::vec3 lightPos  = light->GetPosition();
	const glm::vec3 cubePos   = shadowRes.cubePos;
	const glm::vec3 cameraPos = camera->GetPosition();
	const glm::vec3 cubeAABBLo(cubePos.x - 0.5f, cubePos.y - 0.5f, cubePos.z - 0.5f);
	const glm::vec3 cubeAABBHi(cubePos.x + 0.5f, cubePos.y + 0.5f, cubePos.z + 0.5f);

	camera->ChangeCamRatio(static_cast<float>(kRes), static_cast<float>(kRes));
	const CameraUBOData camUBO = VulkanTestShared::ComputeCameraUBO(*camera);
	const glm::mat4 invProjView = glm::inverse(camUBO.viewProj);

	NEURUS_LOG("[ShadowIntensityTest] cube@("
	           << cubePos.x << "," << cubePos.y << "," << cubePos.z << ")"
	           << " light@(" << lightPos.x << "," << lightPos.y << "," << lightPos.z << ")"
	           << " camera@(" << cameraPos.x << "," << cameraPos.y << "," << cameraPos.z << ")");

	// -------------------------------------------------------------------
	// Step 2: One shared RenderContext for all three passes
	// -------------------------------------------------------------------
	RenderContext ctx{};
	ctx.width = renderExtent.width; ctx.height = renderExtent.height;
	ctx.editor.scene = shadowRes.scene.get();

	// -------------------------------------------------------------------
	// Step 3: Record all passes in a single command buffer
	// -------------------------------------------------------------------
	{
		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		m_shadowDepthPass->Record(*cmd, *m_renderCache, ctx);
		m_shadowIntensityPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 4: Read back R8 shadow intensity from the layered array
	// -------------------------------------------------------------------
	auto& intensityArray = m_renderCache->GetShadowIntensityArray(renderExtent);
	const uint32_t layer = m_renderCache->GetShadowIntensityLayer(lightUID, renderExtent);
	const vk::DeviceSize bufSize = static_cast<vk::DeviceSize>(kRes) * kRes;
	std::vector<uint8_t> u8ShadowData(kRes * kRes);
	{
		auto& cmd = BeginCmd();
		Barrier::Transition(*cmd, intensityArray, ImageState::TransferSrc);
		vk::raii::Buffer stagingBuf(*m_device,
			vk::BufferCreateInfo({}, bufSize, vk::BufferUsageFlagBits::eTransferDst));
		auto memReqs = stagingBuf.getMemoryRequirements();
		uint32_t memType = VulkanTestShared::FindMemoryType(pd, memReqs.memoryTypeBits,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		ASSERT_LT(memType, UINT32_MAX);
		vk::raii::DeviceMemory stagingMem(*m_device, vk::MemoryAllocateInfo(memReqs.size, memType));
		stagingBuf.bindMemory(*stagingMem, 0);
		vk::BufferImageCopy copy{};
		copy.imageSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, layer, 1);
		copy.imageExtent = vk::Extent3D(kRes, kRes, 1);
		cmd.copyImageToBuffer(*intensityArray.ImageHandle(),
		                      vk::ImageLayout::eTransferSrcOptimal, *stagingBuf, copy);
		EndSubmitWait(cmd);
		void* mapped = stagingMem.mapMemory(0, bufSize);
		std::memcpy(u8ShadowData.data(), mapped, bufSize);
		stagingMem.unmapMemory();
	}

	std::vector<float> actualShadow(kRes * kRes);
	for (size_t i = 0; i < kRes * kRes; ++i)
		actualShadow[i] = static_cast<float>(u8ShadowData[i]) / 255.0f;

	// -------------------------------------------------------------------
	// Step 5: Compute expected shadow + pixel-by-pixel validation
	// -------------------------------------------------------------------
	std::vector<float> expectedShadow(kRes * kRes);
	for (uint32_t py = 0; py < kRes; ++py)
		for (uint32_t px = 0; px < kRes; ++px)
			expectedShadow[py * kRes + px] = ComputeExpectedShadow(
				px, py, invProjView, cameraPos, lightPos, cubeAABBLo, cubeAABBHi);

	int shadowedExp = 0, litExp = 0, shadowedAct = 0, litAct = 0;
	int badCount = 0, totalPixels = kRes * kRes;

	for (uint32_t py = 0; py < kRes; ++py)
	{
		for (uint32_t px = 0; px < kRes; ++px)
		{
			const float exp = expectedShadow[py * kRes + px];
			const float act = actualShadow[py * kRes + px];
			if (exp > 0.5f) ++shadowedExp; else ++litExp;
			if (act > 0.5f) ++shadowedAct; else ++litAct;
			if (std::abs(act - exp) > kTolerance && ++badCount <= 5)
				std::cout << "[ShadowIntensity] BAD(" << px << "," << py
				          << "): act=" << act << " exp=" << exp << std::endl;
		}
	}

	std::cout << "[ShadowIntensity] GPU: shadowed=" << shadowedAct
	          << " lit=" << litAct
	          << " | Expected: shadowed=" << shadowedExp
	          << " lit=" << litExp
	          << " | Bad:" << badCount
	          << " (tol=" << kTolerance << ")"
	          << std::endl;

	EXPECT_GT(shadowedAct, 50) << "Expected at least 50 shadowed pixels";
	EXPECT_GT(litAct, 100)     << "Expected at least 100 lit pixels";
	EXPECT_EQ(badCount, 0) << badCount << " of " << totalPixels
		<< " pixel(s) differ from analytically-expected shadow (tol=" << kTolerance << ")";

	// -------------------------------------------------------------------
	// Step 6: Reference image regression
	// -------------------------------------------------------------------
	{
		const std::string refPath =
			neurus::test::ReferencePath::Make("shadow_intensity/ShadowIntensity.png");
		ImageData img(u8ShadowData.data(), kRes, kRes, PixelFormat::R8U);
		ASSERT_TRUE(img.SavePNG(refPath + ".tmp")) << "Failed to save shadow intensity PNG";
		const int refResult = neurus::test::CheckReferenceOrGenerate(refPath, 2);
		if (refResult < 0)
			GTEST_SKIP() << "Reference image generated. Re-run the test to compare.";
		else
			EXPECT_EQ(refResult, 0) << refResult << " pixel(s) differ from reference";
	}
}
