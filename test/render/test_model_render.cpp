/**
 * @file test_model_render.cpp
 * @brief End-to-end GPU test: load OBJ + texture, render G-Buffer → PBR lighting, verify output.
 *
 * Validates the full deferred rendering pipeline:
 *   - Loads a real OBJ model (sphere.obj) via MeshData
 *   - Loads a texture (BAKED.png) via Texture::FromFile
 *   - Creates Material with albedo texture and default PBR params
 *   - Creates Camera, PointLight, Scene with registration
 *   - Runs G-Buffer geometry pass (GeometryPass)
 *   - Runs PBR lighting compute pass (LightingPass)
 *   - Reads back HDR colour output via staging buffer
 *   - Verifies at least one pixel has non-zero RGB value
 *
 * @note Requires a Vulkan 1.4-capable GPU. Skipped in CI without GPU.
 * @note Asset paths are relative to the test executable directory (build/debug/).
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

// Render layer
#include "render/RenderCache.h"
#include "render/UploadManager.h"
#include "render/resources/LightingCache.h"
#include "scene/Material.h"
#include "render/passes/GeometryPass.h"
#include "render/passes/LightingPass.h"
#include "render/RenderContext.h"
#include "render/Texture.h"
#include "asset/data/ImageData.h"
#include "render/Texture.h"
#include "render/buffers/BufferLayout.h"
#include "render/buffers/IndexBuffer.h"
#include "render/buffers/VertexBuffer.h"

// Data layer
#include "asset/data/MeshData.h"

// Scene layer
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <fstream>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace neurus;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

/**
 * @brief GPU test fixture for model rendering through G-Buffer → PBR lighting.
 *
 * Creates a headless Vulkan device, G-Buffer + HDR colour attachments,
 * GeometryPass, LightingPass, Camera, Light, and Scene.
 */
class ModelRenderTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;

		auto& pd = PhysicalDevice();

		// --- Check push-constant size support ---
		const auto& limits = pd.getProperties().limits;
		if (limits.maxPushConstantsSize < sizeof(LightingPushConstants))
		{
			m_hasVulkan = false;
			return;
		}

		// --- Attachment manager (G-Buffer + HDR color + depth) - attachments created lazily ---
		m_renderCache = std::make_unique<RenderCache>(*m_device, pd);
		m_renderCache->SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily));

		// --- Geometry pass ---
		m_geometryPass = std::make_unique<GeometryPass>(
			*m_device, pd);

		// --- Lighting pass ---
		m_lightingPass = std::make_unique<LightingPass>(
			*m_device, pd,
			2u);                          // numSets = kMaxFramesInFlight

		// --- Upload manager for CPU→GPU struct conversion ---
	}

	// --- Constants ---
	static constexpr uint32_t kRenderWidth  = 256;
	static constexpr uint32_t kRenderHeight = 256;

	// --- Render pass infrastructure ---
	std::unique_ptr<RenderCache>  m_renderCache;


	// --- Systems under test ---
	std::unique_ptr<GeometryPass>  m_geometryPass;
	std::unique_ptr<LightingPass>  m_lightingPass;
};

// ===========================================================================
// Tests
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. Constructor - pipeline is created successfully
// ---------------------------------------------------------------------------

TEST_F(ModelRenderTest, Constructor_CreatesValidPipelines)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NE(m_geometryPass, nullptr);
	ASSERT_NE(m_lightingPass, nullptr);
	SUCCEED();
}

// ---------------------------------------------------------------------------
// 2. End-to-end: sphere OBJ → G-Buffer → PBR lighting → non-zero output
// ---------------------------------------------------------------------------

TEST_F(ModelRenderTest, SphereMeshWithPBR_ProducesNonZeroOutput)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	const auto& pd = m_physicalDevices[m_selectedPdIndex];

	// -----------------------------------------------------------------------
	// Step 1: Load the sphere OBJ via MeshData
	// -----------------------------------------------------------------------
	std::string objPath = ResolveAssetPath("res/obj/sphere.obj");

	auto meshData = std::make_shared<MeshData>();
	const bool loaded = meshData->LoadObj(objPath);
	ASSERT_TRUE(loaded) << "Failed to load OBJ: " << objPath;

	const auto& rawMesh = meshData->GetMeshData();
	ASSERT_GT(rawMesh.dataArray.size() / 14, 0u) << "OBJ has no vertices";
	ASSERT_GT(rawMesh.indexArray.size(), 0u) << "OBJ has no indices";

	// -----------------------------------------------------------------------
	// Step 2: Load BAKED.png texture (albedo)
	// -----------------------------------------------------------------------
	std::string texPath = ResolveAssetPath("res/tex/BAKED.png");

	Texture albedoTexture = Texture::FromFile(
		*m_device, pd, m_queue, m_graphicsQueueFamily,
		texPath.c_str(),
		vk::Format::eR8G8B8A8Srgb);

	// Texture loading may fail if file not found - warn but don't abort
	if (!albedoTexture.IsValid())
	{
		std::cerr << "[WARN] Failed to load texture: " << texPath
		          << " - continuing without albedo texture." << std::endl;
	}

	// -----------------------------------------------------------------------
	// Step 3: Create Material with albedo texture and default PBR params
	// -----------------------------------------------------------------------
	auto material = std::make_shared<Material>();
	material->mat_name = "TestMaterial";

	// Set default PBR params: metal=0, rough=0.5 (default from InitParamData)
	material->SetMatParam(Material::MAT_METAL, 0.0f);
	material->SetMatParam(Material::MAT_ROUGH, 0.5f);
	material->SetMatParam(Material::MAT_ALBEDO, glm::vec3(1.0f, 1.0f, 1.0f));

	if (albedoTexture.IsValid())
	{
			// Wrap in shared_ptr for Texture::TextureRes
		auto texRes = std::make_shared<Texture>(std::move(albedoTexture));
		material->SetMatParam(Material::MAT_ALBEDO, texRes);
	}

	// -----------------------------------------------------------------------
	// Step 4: Create Camera (position above-back, looking at origin — Z-up)
	// -----------------------------------------------------------------------
	auto camera = std::make_shared<Camera>(
		800.0f, 600.0f,  // viewport size
		60.0f,            // FOV
		0.1f,             // near
		100.0f);          // far

	camera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
	camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));

	// -----------------------------------------------------------------------
	// Step 5: Create PointLight at position (3, 3, 3) with white color
	// -----------------------------------------------------------------------
	auto light = std::make_shared<Light>(
		LightType::POINTLIGHT,
		10.0f,
		glm::vec3(1.0f, 1.0f, 1.0f));

	light->SetPosition(glm::vec3(3.0f, 3.0f, 3.0f));
	light->light_radius = 0.0f;

	// -----------------------------------------------------------------------
	// Step 6: Create Mesh from loaded data, assign material
	// -----------------------------------------------------------------------
	auto mesh = std::make_shared<Mesh>();
	mesh->o_name = "SphereMesh";
	mesh->o_mesh = meshData;  // share ownership (no move - we copied data above)
	mesh->o_material = material;

	// -----------------------------------------------------------------------
	// Step 7: Create Scene, register camera, mesh, light
	// -----------------------------------------------------------------------
	Scene scene;
	scene.UseCamera(camera);
	scene.ActivateCamera(camera->GetObjectID());
	scene.UseMesh(mesh);
	scene.UseLight(light);

	// --- Pre-register GPU resources before pass recording ---
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	// -----------------------------------------------------------------------
	// Step 8: Camera registered via scene.UseCamera(); passes get it from ctx.scene->GetActiveCamera()
	// -----------------------------------------------------------------------

	// -----------------------------------------------------------------------
	// Step 9: GPU buffers created lazily by RenderCache::GetMeshGPU()
	// on first access during GeometryPass::Record().
	// -----------------------------------------------------------------------

	// -----------------------------------------------------------------------
	// Step 10: Transition G-Buffer attachments to renderable layouts
	// -----------------------------------------------------------------------
	VulkanTestShared::TransitionGbufferToColorAttachment(*m_renderCache, {kRenderWidth, kRenderHeight}, *this);

	// -----------------------------------------------------------------------
	// Step 11: Build RenderContext & record geometry pass (G-Buffer write)
	// -----------------------------------------------------------------------
	RenderContext ctx{
			.width = kRenderWidth, .height = kRenderHeight,
		.frameIndex = 0,
		.editor = { .scene = &scene },
	};

	{
		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, ctx.editor);
		m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -----------------------------------------------------------------------
	// Step 13: Upload light to RenderCache and record lighting pass
	// -----------------------------------------------------------------------
	{
		auto lightDict = m_uploadManager->UploadLighting(scene.light_list);
		m_renderCache->UpdateLighting(lightDict);
	}

	{
		auto& cmd = BeginCmd();
		m_lightingPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
	}

	// -----------------------------------------------------------------------
	// Step 14: Read back HDR colour output
	// -----------------------------------------------------------------------
	std::vector<float> hdrPixels = VulkanTestShared::ReadbackHdrOutput(
		*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily,
		*m_renderCache, kRenderWidth, kRenderHeight);

	// -----------------------------------------------------------------------
	// Step 15: Verify at least one pixel has non-zero RGB value
	// -----------------------------------------------------------------------
	bool foundNonZero = false;
	for (size_t i = 0; i < hdrPixels.size(); i += 4)
	{
		const float r = hdrPixels[i + 0];
		const float g = hdrPixels[i + 1];
		const float b = hdrPixels[i + 2];

		if (r > 0.01f || g > 0.01f || b > 0.01f)
		{
			foundNonZero = true;
			break;
		}
	}

	EXPECT_TRUE(foundNonZero)
		<< "No non-zero pixel found in HDR output after deferred PBR pipeline. "
		<< "The sphere should be illuminated by the point light at ("
		<< light->GetPosition().x << ", " << light->GetPosition().y
		<< ", " << light->GetPosition().z << ") with power "
		<< light->light_power << ".";

	if (!foundNonZero)
	{
		// Dump center pixel and a few surrounding for debugging
		const size_t cx = kRenderWidth / 2;
		const size_t cy = kRenderHeight / 2;

		for (int dy = -2; dy <= 2; ++dy)
		{
			for (int dx = -2; dx <= 2; ++dx)
			{
				const size_t px = cx + dx;
				const size_t py = cy + dy;
				if (px < kRenderWidth && py < kRenderHeight)
				{
					const size_t idx = (py * kRenderWidth + px) * 4;
					std::cerr << "  Pixel(" << px << "," << py << "): RGBA("
					          << hdrPixels[idx + 0] << ", "
					          << hdrPixels[idx + 1] << ", "
					          << hdrPixels[idx + 2] << ", "
					          << hdrPixels[idx + 3] << ")" << std::endl;
				}
			}
		}
	}
}
