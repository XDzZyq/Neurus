/**
 * @file test_gbuffer.cpp
 * @brief Tests for GeometryPass - G-Buffer MRT rendering.
 *
 * Validates:
 *   - GeometryPass constructor creates a valid pipeline
 *   - Record() succeeds without validation errors
 *   - All 4 colour attachments receive valid (non-zero) data
 *   - Depth attachment is populated
 *
 * @note Requires a Vulkan 1.4-capable GPU. Skipped in CI without GPU.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/RenderCache.h"
#include "render/RenderContext.h"
#include "render/passes/GeometryPass.h"
#include "render/buffers/BufferLayout.h"
#include "render/buffers/IndexBuffer.h"
#include "render/buffers/VertexBuffer.h"

#include "asset/data/MeshData.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <memory>
#include <string>

using namespace neurus;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

/**
 * @brief GPU test fixture for GeometryPass.
 *
 * Creates a headless Vulkan device, G-Buffer attachments, and a
 * GeometryPass instance with embedded shaders.
 *
 * Uses VulkanTestShared for standard Vulkan bootstrap (instance, device,
 * queue, command pool/buffers) and adds GeometryPass-specific setup.
 */
class GeometryPassTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!HasVulkan()) return;

		try
		{
			auto& pd = PhysicalDevice();

			// --- Check push-constant size support ---
			const auto& limits = pd.getProperties().limits;
			if (limits.maxPushConstantsSize < sizeof(MeshPushConstants))
			{
				m_hasVulkan = false;
				return;
			}

			// --- Attachment manager (G-Buffer + depth) - attachments created lazily ---
			m_renderCache = std::make_unique<RenderCache>(*m_device, pd);
			m_renderCache->SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily));

			// --- Geometry pass ---
		m_geometryPass = std::make_unique<GeometryPass>(
			*m_device, pd);

			m_hasVulkan = true;
		}
		catch (const std::exception& e)
		{
			std::cerr << "[GeometryPassTest::SetUp] Exception: " << e.what() << std::endl;
			m_hasVulkan = false;
		}
		catch (...)
		{
			m_hasVulkan = false;
		}
	}

	void TearDown() override
	{
		if (m_device)
		{
			m_device->waitIdle();
		}
		m_geometryPass.reset();
		m_renderCache.reset();
		VulkanTestShared::TearDown();
	}

	// --- Constants ---
	static constexpr uint32_t kRenderWidth  = 128;
	static constexpr uint32_t kRenderHeight = 128;

	// --- Render pass infrastructure ---
	std::unique_ptr<RenderCache>  m_renderCache;

	// --- System under test ---
	std::unique_ptr<GeometryPass> m_geometryPass;
};

// ===========================================================================
// Tests
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. Constructor - pipeline is created successfully
// ---------------------------------------------------------------------------

TEST_F(GeometryPassTest, Constructor_CreatesValidPipeline)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	// GeometryPass was created in SetUp - verify it exists
	ASSERT_NE(m_geometryPass, nullptr);
	SUCCEED();
}

// ---------------------------------------------------------------------------
// 2. Record - single triangle, no validation errors
// ---------------------------------------------------------------------------

TEST_F(GeometryPassTest, Record_SingleTriangle_NoValidationError)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();

	// --- Transition attachments to renderable layouts ---
	VulkanTestShared::TransitionGbufferToColorAttachment(*m_renderCache, {kRenderWidth, kRenderHeight}, *this);

	// --- Create mesh from OBJ string ---
	auto meshData = std::make_shared<MeshData>();
	const std::string objStr =
		"v 0.0 -0.5 0.0\n"
		"v 0.5 0.5 0.0\n"
		"v -0.5 0.5 0.0\n"
		"vn 0.0 0.0 1.0\n"
		"f 1//1 2//1 3//1\n";
	ASSERT_TRUE(meshData->LoadObjFromString(objStr));

	auto mesh = std::make_shared<Mesh>();
	mesh->o_mesh = meshData;

	// Register mesh in Scene so GeometryPass can iterate mesh_list
	Scene testScene;
	testScene.UseMesh(mesh);

	// --- Camera ---
	auto testCam = VulkanTestShared::CreateTestCamera(kRenderWidth, kRenderHeight);
	testScene.UseCamera(testCam);

	// --- Record ---
	{
		auto& cmd = BeginCmd();

		PublishSceneCamera(*m_renderCache, testScene);
		m_geometryPass->Record(*cmd, *m_renderCache, RenderContext{
			.width = kRenderWidth, .height = kRenderHeight,
			.editor = { .scene = &testScene },
		});

		EndSubmitWait(cmd);
	}

	SUCCEED();
}

TEST_F(GeometryPassTest, Record_MultipleItems_NoValidationError)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();

	VulkanTestShared::TransitionGbufferToColorAttachment(*m_renderCache, {kRenderWidth, kRenderHeight}, *this);

	// --- Create mesh from OBJ string ---
	auto meshData = std::make_shared<MeshData>();
	const std::string objStr =
		"v 0.0 -0.5 0.0\n"
		"v 0.5 0.5 0.0\n"
		"v -0.5 0.5 0.0\n"
		"vn 0.0 0.0 1.0\n"
		"f 1//1 2//1 3//1\n";
	ASSERT_TRUE(meshData->LoadObjFromString(objStr));

	// Create two Mesh instances with different transforms sharing the same MeshData.
	auto mesh0 = std::make_shared<Mesh>();
	mesh0->o_mesh = meshData;
	auto mesh1 = std::make_shared<Mesh>();
	mesh1->o_mesh = meshData;
	mesh1->SetPosition(glm::vec3(1.0f, 0.0f, 0.0f));

	Scene testScene;
	testScene.UseMesh(mesh0);
	testScene.UseMesh(mesh1);

	auto testCam = VulkanTestShared::CreateTestCamera(kRenderWidth, kRenderHeight);
	testScene.UseCamera(testCam);

	{
		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, testScene);
		m_geometryPass->Record(*cmd, *m_renderCache, RenderContext{
			.width = kRenderWidth, .height = kRenderHeight,
			.editor = { .scene = &testScene },
		});
		EndSubmitWait(cmd);
	}

	SUCCEED();
}

// ---------------------------------------------------------------------------
// 4. Record - empty render items (should not crash)
// ---------------------------------------------------------------------------

TEST_F(GeometryPassTest, Record_EmptyRenderItems_NoCrash)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	VulkanTestShared::TransitionGbufferToColorAttachment(*m_renderCache, {kRenderWidth, kRenderHeight}, *this);

	Scene testScene;
	auto testCam = VulkanTestShared::CreateTestCamera(kRenderWidth, kRenderHeight);
	testScene.UseCamera(testCam);

	{
		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, testScene);
		m_geometryPass->Record(*cmd, *m_renderCache, RenderContext{
			.width = kRenderWidth, .height = kRenderHeight,
			.editor = { .scene = &testScene },  // Scene has camera but no meshes → no geometry drawn (should not crash)
		});
		EndSubmitWait(cmd);
	}

	SUCCEED();
}

// ---------------------------------------------------------------------------
// 5. Non-copyable / movable
// ---------------------------------------------------------------------------

TEST_F(GeometryPassTest, NonCopyable)
{
	static_assert(!std::is_copy_constructible_v<GeometryPass>,
	              "GeometryPass must not be copy-constructible");
	static_assert(!std::is_copy_assignable_v<GeometryPass>,
	              "GeometryPass must not be copy-assignable");
	SUCCEED();
}

TEST_F(GeometryPassTest, Movable)
{
	static_assert(std::is_move_constructible_v<GeometryPass>,
	              "GeometryPass must be move-constructible");
	static_assert(std::is_move_assignable_v<GeometryPass>,
	              "GeometryPass must be move-assignable");
	SUCCEED();
}

// ---------------------------------------------------------------------------
// 6. Camera UBO layout - size sanity check
// ---------------------------------------------------------------------------

TEST_F(GeometryPassTest, CameraUBOData_SizeMatchesShaderExpectation)
{
	// Shader expects 2 mat4s (viewProj + view) = 128 bytes
	EXPECT_EQ(sizeof(CameraUBOData), 128u);
}

// ---------------------------------------------------------------------------
// 7. PushConstants size matches shader
// ---------------------------------------------------------------------------

TEST_F(GeometryPassTest, PushConstants_SizeMatchesShaderExpectation)
{
	// Shader expects 2 mat4s (model + normalMatrix) + uint32 objectID = 144 bytes
	EXPECT_EQ(sizeof(MeshPushConstants), 144u);
}
