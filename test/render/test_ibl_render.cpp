/**
 * @file test_ibl_render.cpp
 * @brief Reference-image regression test for IBL rendering.
 *
 * Generates a procedural colourful gradient HDR equirectangular image,
 * creates IBL cubemaps via IBLPass, renders an icosphere through the
 * deferred pipeline (geometry + PBR lighting with IBL), captures the
 * HDRColor output, and compares pixel‑wise against a reference PNG.
 *
 * On first run the reference image does not exist — the test generates it
 * automatically and reports SKIPPED.  Subsequent runs compare and FAIL on
 * any pixel difference exceeding the allowed tolerance.
 *
 * @note Requires a Vulkan 1.4‑capable GPU.  Skipped in CI without GPU.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

// --- Render layer ---
#include "render/RenderCache.h"
#include "render/RenderContext.h"
#include "render/UploadManager.h"
#include "render/resources/LightingCache.h"
#include "render/passes/GeometryPass.h"
#include "render/passes/IBLPass.h"
#include "render/passes/LightingPass.h"
#include "render/Image.h"
#include "scene/Material.h"
#include "render/Screenshot.h"
#include "render/buffers/IndexBuffer.h"
#include "render/buffers/VertexBuffer.h"

// --- Asset layer ---
#include "asset/data/MeshData.h"
#include "asset/data/ImageData.h"

// --- Scene layer ---
#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include "shared/TestReferenceImage.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace neurus;

// ---------------------------------------------------------------------------
// Procedural colourful gradient equirectangular image
//
// Horizontal: red (left) — blue (right)
// Vertical:   green mid-band, white equator
// Poles fade to dark
// Alpha = 1.0 everywhere
// ---------------------------------------------------------------------------
static std::vector<float> GenerateColorfulGradient(uint32_t width, uint32_t height)
{
	std::vector<float> pixels(static_cast<size_t>(width) * height * 4, 0.0f);

	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
			const float u = static_cast<float>(x) / static_cast<float>(width);
			const float v = static_cast<float>(y) / static_cast<float>(height);

			// Horizontal: red — blue
			float r = 1.0f - u;
			float g = 0.0f;
			float b = u;

			// Green vertical mid-band
			const float distFromEquator = std::abs(v - 0.5f) * 2.0f; // 0 at equator, 1 at poles
			const float greenStrength = std::max(0.0f, 1.0f - distFromEquator * 1.2f);
			g += greenStrength;

			// White equator blend
			const float equatorWeight = std::max(0.0f, 1.0f - distFromEquator * 1.5f);
			const float white = equatorWeight * 1.0f;
			r = r * (1.0f - equatorWeight) + white;
			g = g * (1.0f - equatorWeight) + white;
			b = b * (1.0f - equatorWeight) + white;

			// Fade to dark at poles
			const float poleFalloff = 1.0f - distFromEquator * 0.8f;
			r *= poleFalloff;
			g *= poleFalloff;
			b *= poleFalloff;

			pixels[idx + 0] = r;
			pixels[idx + 1] = g;
			pixels[idx + 2] = b;
			pixels[idx + 3] = 1.0f; // alpha
		}
	}

	return pixels;
}

// ---------------------------------------------------------------------------
// Loads sphere.obj and scales positions 0.25x (matches original manual
// vertex extraction scaling). Shared by both tests in this file.
// ---------------------------------------------------------------------------
static std::shared_ptr<MeshData> LoadScaledSphere(const std::string& objPath)
{
	auto meshData = std::make_shared<MeshData>();
	if (!meshData->LoadObj(objPath))
		return nullptr;

	auto& raw = const_cast<MeshData::ByteArray&>(meshData->GetMeshData());
	const size_t vertexCount = raw.dataArray.size() / 14;
	if (vertexCount == 0 || raw.indexArray.empty())
		return meshData;

	for (size_t i = 0; i < vertexCount; ++i)
	{
		raw.dataArray[i * 14 + 0] *= 0.25f;
		raw.dataArray[i * 14 + 1] *= 0.25f;
		raw.dataArray[i * 14 + 2] *= 0.25f;
	}
	return meshData;
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class IBLRenderTest : public VulkanTestShared
{
protected:
	static constexpr uint32_t kRenderWidth  = 256;
	static constexpr uint32_t kRenderHeight = 256;
	static constexpr uint32_t kEquiWidth    = 512;
	static constexpr uint32_t kEquiHeight   = 256;

	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;

		auto& dev = *m_device;
		auto& pd  = PhysicalDevice();

		// --- Render pass infrastructure ---
		m_renderCache = std::make_unique<RenderCache>(dev, pd);
		m_renderCache->SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily));

		// --- Geometry pass ---
		m_geometryPass = std::make_unique<GeometryPass>(
			dev, pd);

		// --- Upload manager for CPU→GPU struct conversion ---

		// --- Lighting pass ---
		m_lightingPass = std::make_unique<LightingPass>(
			dev, pd,
			1u);  // single frame

		// --- IBL pass ---
		m_iblPass = std::make_unique<IBLPass>(
			dev, pd);

	// --- Create Environment (CPU-only) and generate IBL cubemaps ---
	m_env = std::make_shared<Environment>();

	// Set up procedural colourful gradient equirect data
	auto gradientPixels = GenerateColorfulGradient(kEquiWidth, kEquiHeight);
	auto imgData = std::make_shared<ImageData>(
		gradientPixels.data(), kEquiWidth, kEquiHeight, PixelFormat::RGBA32F);
	m_env->SetEquirectData(imgData);

	// Inline EnvironmentGPU creation (was RenderCache::CreateEnvironmentGPU)
	{
		auto& dev = *m_device;
		auto& pd = PhysicalDevice();
		const int envId = m_env->GetObjectID();

		// 1. Upload equirect to GPU
		auto equirectImage = std::make_shared<Image>(Image::FromImageData(
			dev, pd, m_queue, m_graphicsQueueFamily,
			*imgData, "Env_Equirect",
			vk::ImageUsageFlagBits::eStorage));
		ASSERT_NE(equirectImage->State(), ImageState::Invalid) << "Failed to upload equirect";

		// 2. Create cubemap Images
		constexpr uint32_t kDiffuseRes  = 64;
		constexpr uint32_t kSpecularRes = 2048;
		constexpr uint32_t kSpecularMips = 8;
		const vk::ImageUsageFlags cubeUsage =
			vk::ImageUsageFlagBits::eStorage |
			vk::ImageUsageFlagBits::eSampled |
			vk::ImageUsageFlagBits::eTransferSrc;

		auto diffuseImage = std::make_unique<Image>(
			dev, pd, vk::Extent2D{kDiffuseRes, kDiffuseRes},
			vk::Format::eR32G32B32A32Sfloat, cubeUsage,
			/*mipLevels=*/1, Image::ImageType::eCube, "Env_DiffuseCubemap");

		auto specularImage = std::make_unique<Image>(
			dev, pd, vk::Extent2D{kSpecularRes, kSpecularRes},
			vk::Format::eR32G32B32A32Sfloat, cubeUsage,
			/*mipLevels=*/kSpecularMips, Image::ImageType::eCube, "Env_SpecularCubemap");

		// 3. Create cubemap samplers
		vk::SamplerCreateInfo samplerCI(
			{}, vk::Filter::eLinear, vk::Filter::eLinear,
			vk::SamplerMipmapMode::eLinear,
			vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge,
			0.0f, VK_FALSE, 0.0f, VK_FALSE,
			vk::CompareOp::eAlways, 0.0f, 1.0f,
			vk::BorderColor::eFloatTransparentBlack, VK_FALSE);
		auto diffuseSampler = vk::raii::Sampler(dev, samplerCI);
		samplerCI.setMaxLod(static_cast<float>(kSpecularMips));
		auto specularSampler = vk::raii::Sampler(dev, samplerCI);

		// 4. Run IBL convolution
		m_iblPass->Generate(m_queue, m_graphicsQueueFamily, *equirectImage, *diffuseImage, *specularImage);

		// 5. Wrap in Textures and register
		EnvironmentGPU gpu;
		gpu.diffuseTexture = std::make_unique<Texture>(
			Texture::FromImage(std::move(diffuseImage), std::move(diffuseSampler)));
		gpu.specularTexture = std::make_unique<Texture>(
			Texture::FromImage(std::move(specularImage), std::move(specularSampler)));

		m_renderCache->UseEnvironmentGPU(envId, std::move(gpu));
	}
	}

	void TearDown() override
	{
		VulkanTestShared::TearDown();
	}

	// --- Render pass infrastructure ---
	std::unique_ptr<RenderCache>  m_renderCache;

	std::unique_ptr<GeometryPass>       m_geometryPass;
	std::unique_ptr<LightingPass>       m_lightingPass;
	std::unique_ptr<IBLPass>            m_iblPass;
	// --- IBL environment (CPU-only now; GPU resources in RenderCache) ---
	std::shared_ptr<Environment>        m_env;
};

// ===========================================================================
// IBL Render Reference Image Regression Test
// ===========================================================================

TEST_F(IBLRenderTest, IBLRender_MatchesReferenceImage)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();

	// -------------------------------------------------------------------
	// Step 1: Load sphere OBJ (scaled 0.25x)
	// -------------------------------------------------------------------
	std::string objPath = ResolveAssetPath("res/obj/sphere.obj");
	auto meshData = LoadScaledSphere(objPath);
	ASSERT_NE(meshData, nullptr) << "Failed to load OBJ: " << objPath;
	ASSERT_GT(meshData->GetMeshData().dataArray.size() / 14, 0u);
	ASSERT_GT(meshData->GetMeshData().indexArray.size(), 0u);

	// -------------------------------------------------------------------
	// Step 2: Create camera (60fov, pos (0, -5, 2), looking at origin — Z-up)
	// -------------------------------------------------------------------
	auto camera = std::make_shared<Camera>(
		static_cast<float>(kRenderWidth),
		static_cast<float>(kRenderHeight),
		60.0f, 0.1f, 100.0f);
	camera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
	camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	// (camUBO removed; passes get camera from ctx.scene->GetActiveCamera())

	// -------------------------------------------------------------------
	// Step 3: Create point light (for mixed direct + IBL lighting)
	// -------------------------------------------------------------------
	auto light = std::make_shared<Light>(LightType::POINTLIGHT, 10.0f, glm::vec3(1.0f));
	light->SetPosition(glm::vec3(2.0f, 2.0f, 2.0f));
	light->light_radius = 10.0f;

	// -------------------------------------------------------------------
	// Step 4: Build mesh + material + upload to GPU
	// -------------------------------------------------------------------
	auto material = std::make_shared<Material>();
	material->SetMatParam(Material::MAT_METAL, 0.0f);
	material->SetMatParam(Material::MAT_ROUGH, 0.5f);
	material->SetMatParam(Material::MAT_ALBEDO, glm::vec3(1.0f, 1.0f, 1.0f));

	auto mesh = std::make_shared<Mesh>();
	mesh->o_mesh = meshData;
	mesh->o_material = material;

	// -------------------------------------------------------------------
	// Step 7: Transition G-Buffer, build RenderContext, create scene
	// -------------------------------------------------------------------
	VulkanTestShared::TransitionGbufferToColorAttachment(*m_renderCache, {kRenderWidth, kRenderHeight}, *this);

	Scene scene;
				scene.UseMesh(mesh);
				scene.UseLight(light);
				scene.UseCamera(camera);
				scene.env_list[m_env->GetObjectID()] = m_env;

	// Pre-register GPU resources before pass recording
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	auto lightDict = m_uploadManager->UploadLighting(scene.light_list);
	m_renderCache->UpdateLighting(lightDict);

	RenderContext ctx{
		.width = kRenderWidth,
		.height = kRenderHeight,
		.frameIndex = 0,
		.editor = { .scene = &scene },
	};

	// --- Record geometry pass ---
	{
		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 8: Record lighting pass (with IBL from scene Environment)
	// -------------------------------------------------------------------
	{
		auto& cmd = BeginCmd();
		m_lightingPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 9: Capture HDRColor & compare with reference
	// -------------------------------------------------------------------
	const std::string refPath = neurus::test::ReferencePath::Make("ibl/ibl_render.png");
	const std::string tmpPath = refPath + ".tmp";

	Image& hdrColor = m_renderCache->GetAttachment(AttachmentName::HDRColor, {kRenderWidth, kRenderHeight});
	const bool captured = Screenshot::CaptureAttachment(
		*m_device, pd, m_queue, m_graphicsQueueFamily,
		hdrColor, tmpPath);

	ASSERT_TRUE(captured) << "Failed to capture HDRColor attachment";

	// Tolerance widened to account for platform-specific float/texture-sampling
	// differences (GPU vendor/driver, MoltenVK vs. native Vulkan) around
	// specular highlights: +/-4 per channel, up to 1% of pixels may differ.
	const int result = neurus::test::CheckReferenceOrGenerate(refPath, 4, 0.01);
	if (result < 0)
		GTEST_SKIP() << "Reference image generated. Re-run the test to compare.";
	EXPECT_EQ(result, 0) << result << " pixel(s) differ in IBL render (threshold: 4 per channel, 1% pixel budget).";
}

// ===========================================================================
// 5. Reload test — validates GPU resource lifetime across destroy/recreate
// ===========================================================================

TEST_F(IBLRenderTest, Reload_Environment_NoValidationErrors)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& dev = *m_device;
	auto& pd  = PhysicalDevice();

	// ================================================================
	// Phase 1 — Set up scene + IBL and render Frame 1
	// ================================================================

	// --- Load sphere OBJ (scaled 0.25x) ---
	std::string objPath = ResolveAssetPath("res/obj/sphere.obj");
	auto meshData = LoadScaledSphere(objPath);
	ASSERT_NE(meshData, nullptr) << "Failed to load OBJ: " << objPath;
	ASSERT_GT(meshData->GetMeshData().dataArray.size() / 14, 0u);
	ASSERT_GT(meshData->GetMeshData().indexArray.size(), 0u);

	// --- Camera (60fov, pos (0,-5,2), looking at origin — Z-up) ---
	auto camera = std::make_shared<Camera>(
		static_cast<float>(kRenderWidth),
		static_cast<float>(kRenderHeight),
		60.0f, 0.1f, 100.0f);
	camera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
	camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	// --- Light ---
	auto light = std::make_shared<Light>(LightType::POINTLIGHT, 10.0f, glm::vec3(1.0f));
	light->SetPosition(glm::vec3(2.0f, 2.0f, 2.0f));
	light->light_radius = 10.0f;

	// --- Material ---
	auto material = std::make_shared<Material>();
	material->SetMatParam(Material::MAT_METAL, 0.0f);
	material->SetMatParam(Material::MAT_ROUGH, 0.5f);
	material->SetMatParam(Material::MAT_ALBEDO, glm::vec3(1.0f, 1.0f, 1.0f));

	// --- Mesh ---
	auto mesh = std::make_shared<Mesh>();
	mesh->o_mesh = meshData;
	mesh->o_material = material;

	// --- Render Frame 1 (IBL active) ---
	{
		VulkanTestShared::TransitionGbufferToColorAttachment(*m_renderCache, {kRenderWidth, kRenderHeight}, *this);

	Scene scene;
				scene.UseMesh(mesh);
				scene.UseLight(light);
				scene.UseCamera(camera);
				scene.env_list[m_env->GetObjectID()] = m_env;
				// Pre-register GPU resources before pass recording
				VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
				VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
				auto lightDict = m_uploadManager->UploadLighting(scene.light_list);
				m_renderCache->UpdateLighting(lightDict);

			RenderContext ctx{
				.width = kRenderWidth, .height = kRenderHeight,
				.frameIndex = 0,
				.editor = { .scene = &scene },
			};

		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);

		auto& cmd2 = BeginCmd();
		m_lightingPass->Record(*cmd2, *m_renderCache, ctx);
		EndSubmitWait(cmd2);
	}

	// ================================================================
	// Phase 2 — Simulate project reload: destroy then recreate
	// ================================================================

	// 2a. Wait for all GPU work to finish
	m_device->waitIdle();

	// 2b. Destroy Environment (LightingPass reads IBL from scene per-frame,
	//     so no ResetIBLResources call needed – the old Environment will be
	//     replaced with a fresh one after recreation).

	// 2c. Destroy render pass objects (reverse order of creation).
	//     Unique_ptr destructors handle Vulkan resource teardown.
	//     This simulates destroying DeferredRenderer on project close.
	SCOPED_TRACE("Destroy passes");
	m_iblPass.reset();
	m_lightingPass.reset();
	m_geometryPass.reset();
	m_renderCache.reset();

	// 2d. Destroy IBL GPU resources (Environment + GPU resources in RenderCache).
	//     RenderCache::Clean() handles GPU resource teardown; the Environment
	//     (CPU-only) must be re-created.
	SCOPED_TRACE("Destroy IBL resources");
	m_env.reset();

	// 2e. Recreate RenderCache + passes (simulating renderer init).
	SCOPED_TRACE("Recreate passes");
	m_renderCache = std::make_unique<RenderCache>(dev, pd);
	m_renderCache->SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily));

	m_geometryPass = std::make_unique<GeometryPass>(
		dev, pd);

	m_lightingPass = std::make_unique<LightingPass>(
		dev, pd, 1u);

	m_iblPass = std::make_unique<IBLPass>(
		dev, pd);

	// 2f. Re-create Environment (CPU-only) + generate IBL cubemaps.
	SCOPED_TRACE("Recreate IBL resources");
	m_env = std::make_shared<Environment>();
	std::shared_ptr<ImageData> imgData; // moved out of narrow scope for inline IBL creation below
	{
		auto gradientPixels = GenerateColorfulGradient(kEquiWidth, kEquiHeight);
		imgData = std::make_shared<ImageData>(
			gradientPixels.data(), kEquiWidth, kEquiHeight, PixelFormat::RGBA32F);
		m_env->SetEquirectData(imgData);
	}

	// 2g. Inline EnvironmentGPU creation (was RenderCache::CreateEnvironmentGPU)
	SCOPED_TRACE("Generate IBL cubemaps");
	{
		auto& device = *m_device;
		auto& physDev = PhysicalDevice();
		const int envId = m_env->GetObjectID();

		// 1. Upload equirect to GPU
		auto equirectImage = std::make_shared<Image>(Image::FromImageData(
			device, physDev, m_queue, m_graphicsQueueFamily,
			*imgData, "Env_Equirect",
			vk::ImageUsageFlagBits::eStorage));
		ASSERT_NE(equirectImage->State(), ImageState::Invalid) << "Failed to upload equirect";

		// 2. Create cubemap Images
		constexpr uint32_t kDiffuseRes  = 64;
		constexpr uint32_t kSpecularRes = 2048;
		constexpr uint32_t kSpecularMips = 8;
		const vk::ImageUsageFlags cubeUsage =
			vk::ImageUsageFlagBits::eStorage |
			vk::ImageUsageFlagBits::eSampled |
			vk::ImageUsageFlagBits::eTransferSrc;

		auto diffuseImage = std::make_unique<Image>(
			device, physDev, vk::Extent2D{kDiffuseRes, kDiffuseRes},
			vk::Format::eR32G32B32A32Sfloat, cubeUsage,
			/*mipLevels=*/1, Image::ImageType::eCube, "Env_DiffuseCubemap");

		auto specularImage = std::make_unique<Image>(
			device, physDev, vk::Extent2D{kSpecularRes, kSpecularRes},
			vk::Format::eR32G32B32A32Sfloat, cubeUsage,
			/*mipLevels=*/kSpecularMips, Image::ImageType::eCube, "Env_SpecularCubemap");

		// 3. Create cubemap samplers
		vk::SamplerCreateInfo samplerCI(
			{}, vk::Filter::eLinear, vk::Filter::eLinear,
			vk::SamplerMipmapMode::eLinear,
			vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge,
			0.0f, VK_FALSE, 0.0f, VK_FALSE,
			vk::CompareOp::eAlways, 0.0f, 1.0f,
			vk::BorderColor::eFloatTransparentBlack, VK_FALSE);
		auto diffuseSampler = vk::raii::Sampler(device, samplerCI);
		samplerCI.setMaxLod(static_cast<float>(kSpecularMips));
		auto specularSampler = vk::raii::Sampler(device, samplerCI);

		// 4. Run IBL convolution
		m_iblPass->Generate(m_queue, m_graphicsQueueFamily, *equirectImage, *diffuseImage, *specularImage);

		// 5. Wrap in Textures and register
		EnvironmentGPU gpu;
		gpu.diffuseTexture = std::make_unique<Texture>(
			Texture::FromImage(std::move(diffuseImage), std::move(diffuseSampler)));
		gpu.specularTexture = std::make_unique<Texture>(
			Texture::FromImage(std::move(specularImage), std::move(specularSampler)));

		m_renderCache->UseEnvironmentGPU(envId, std::move(gpu));
	}

	// ================================================================
	// Phase 3 — Render Frame 2 (after reload, IBL active again)
	// ================================================================

	SCOPED_TRACE("Render Frame 2");
	{
		VulkanTestShared::TransitionGbufferToColorAttachment(*m_renderCache, {kRenderWidth, kRenderHeight}, *this);

	Scene scene;
				scene.UseMesh(mesh);
				scene.UseLight(light);
				scene.UseCamera(camera);
				scene.env_list[m_env->GetObjectID()] = m_env;
				// Pre-register GPU resources before pass recording
				VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
				VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
				auto lightDict = m_uploadManager->UploadLighting(scene.light_list);
				m_renderCache->UpdateLighting(lightDict);

			RenderContext ctx{
				.width = kRenderWidth, .height = kRenderHeight,
				.frameIndex = 0,
				.editor = { .scene = &scene },
			};

		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);

		auto& cmd2 = BeginCmd();
		m_lightingPass->Record(*cmd2, *m_renderCache, ctx);
		EndSubmitWait(cmd2);
	}

	// If we reached here without crashing or triggering Vulkan validation
	// errors, the GPU resource lifetime fix is verified.
	SUCCEED() << "Reload cycle completed without validation errors or crashes.";
}
