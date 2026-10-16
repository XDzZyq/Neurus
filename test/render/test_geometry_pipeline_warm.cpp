/**
 * @file test_geometry_pipeline_warm.cpp
 * @brief Offscreen GPU coverage for GeometryPass per-mesh pipeline warming.
 *
 * GeometryPass::Record() only LOOKS pipelines up; EnsureMeshPipeline() is the
 * only place one is built. Both shipped scenes carry o_shaderId == 0 on every
 * mesh, so no reference image ever exercises a custom per-mesh Shader. These
 * tests are that coverage: they drive a mesh that carries a compiled custom
 * Shader through the real offscreen G-Buffer path and assert the warm/lookup
 * contract through the public PipelineCache surface and EnsureMeshPipeline's
 * return value.
 *
 * @note Requires a Vulkan 1.4-capable GPU. Skipped in CI without GPU.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/RenderCache.h"
#include "render/RenderContext.h"
#include "render/passes/GeometryPass.h"
#include "render/resources/LightingCache.h"
#include "render/resources/PipelineCache.h"
#include "render/shaders/RenderShader.h"

#include "asset/data/MeshData.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include <memory>

using namespace neurus;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

/**
 * @brief Offscreen GPU fixture driving GeometryPass's per-mesh pipeline path.
 *
 * Mirrors GeometryPassTest's bootstrap: headless device, RenderCache with
 * lazily created G-Buffer attachments, and a GeometryPass. Adds helpers to
 * build a compiled custom Shader, a sphere mesh carrying it, and to record one
 * G-Buffer frame the way DeferredRenderer would.
 */
class GeometryPipelineWarmTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;

		try
		{
			auto& pd = PhysicalDevice();
			m_renderCache = std::make_unique<RenderCache>(*m_device, pd);
			m_renderCache->SetLightingCache(std::make_unique<LightingCache>(
				*m_device, pd, m_queue, m_graphicsQueueFamily));
			m_geometryPass = std::make_unique<GeometryPass>(*m_device, pd);
		}
		catch (const std::exception& e)
		{
			std::cerr << "[GeometryPipelineWarmTest::SetUp] " << e.what() << std::endl;
			m_hasVulkan = false;
		}
	}

	void TearDown() override
	{
		if (m_device) m_device->waitIdle();
		m_geometryPass.reset();
		m_renderCache.reset();
		VulkanTestShared::TearDown();
	}

	// Builds a RenderShader from the gbuffer sources and compiles it to SPIR-V.
	// SPIR-V generation is device-free, so this is the same real compile the
	// Editor's create-shader flow runs. @p compile = false leaves the stages
	// without SPIR-V, the "still being edited" state EnsureMeshPipeline treats
	// as a non-error miss.
	std::shared_ptr<RenderShader> MakeShader(const char* name, bool compile = true)
	{
		auto shader = std::make_shared<RenderShader>(
			name,
			ResolveAssetPath("res/shaders/render/gbuffer.vert"),
			ResolveAssetPath("res/shaders/render/gbuffer.frag"));
		if (compile)
		{
			shader->ParseAndGenerate();
			shader->CompileToSpv();
		}
		return shader;
	}

	// A sphere mesh carrying @p shader as its per-mesh Shader.
	std::shared_ptr<Mesh> MakeMesh(std::shared_ptr<Shader> shader)
	{
		auto meshData = std::make_shared<MeshData>();
		meshData->LoadObj(ResolveAssetPath("res/obj/sphere.obj"));

		auto mesh = std::make_shared<Mesh>();
		mesh->o_mesh = meshData;
		mesh->o_shader = std::move(shader);
		return mesh;
	}

	// Registers an active camera, uploads mesh GPU buffers, transitions the
	// G-Buffer and records one GeometryPass frame - exactly the per-frame work
	// DeferredRenderer::recordFrame() does around a single pass.
	PassStats RecordFrame(Scene& scene)
	{
		VulkanTestShared::EnsureMeshesUploaded(
			*m_renderCache, scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
		VulkanTestShared::TransitionGbufferToColorAttachment(
			*m_renderCache, {kWidth, kHeight}, *this);

		RenderContext ctx{
			.scene = &scene,
			.width = kWidth, .height = kHeight,
			.frameIndex = 0,
		};

		auto& cmd = BeginCmd();
		PublishSceneCamera(*m_renderCache, ctx);
		PassStats stats = m_geometryPass->Record(*cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
		return stats;
	}

	// Registers an active camera and the given meshes into @p scene. Scene is a
	// UID (non-movable), so the caller owns it and we populate in place.
	void BuildScene(Scene& scene, std::initializer_list<std::shared_ptr<Mesh>> meshes)
	{
		auto camera = CreateTestCamera(kWidth, kHeight);
		scene.UseCamera(camera);
		scene.ActivateCamera(camera->GetObjectID());
		for (const auto& m : meshes) scene.UseMesh(m);
	}

	static constexpr uint32_t kWidth  = 128;
	static constexpr uint32_t kHeight = 128;

	std::unique_ptr<RenderCache>  m_renderCache;
	std::unique_ptr<GeometryPass> m_geometryPass;
};

// ===========================================================================
// Tests
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. Negative: Record never compiles. An unwarmed custom Shader leaves the
//    cache empty and the mesh draws with the pass default.
// ---------------------------------------------------------------------------

TEST_F(GeometryPipelineWarmTest, Record_DoesNotCompile_WhenShaderUnwarmed)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("UnwarmedShader");
	const int shaderId = shader->GetObjectID();
	const int version  = shader->GetVersion();

	Scene scene;
	BuildScene(scene, {MakeMesh(shader)});

	// No EnsureMeshPipeline() call: Record must not build one on our behalf.
	PassStats stats = RecordFrame(scene);

	PipelineCache& pipelines = m_renderCache->GetPipelineCache();
	// Record compiled nothing - reintroducing compilation inside Record() would
	// make this entry appear and fail the assertion.
	EXPECT_EQ(pipelines.GetPipeline(shaderId, version), nullptr);
	EXPECT_EQ(pipelines.Get(shaderId), nullptr);
	// The mesh still drew: it fell back to the pass default pipeline.
	EXPECT_EQ(stats.drawCalls, 1u);
}

// ---------------------------------------------------------------------------
// 2. Warm, then hit: EnsureMeshPipeline builds and caches; the lookup then
//    finds it and a second record succeeds.
// ---------------------------------------------------------------------------

TEST_F(GeometryPipelineWarmTest, EnsureMeshPipeline_WarmsThenRecordHits)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("WarmShader");
	const int shaderId = shader->GetObjectID();
	const int version  = shader->GetVersion();

	Scene scene;
	BuildScene(scene, {MakeMesh(shader)});

	PipelineCache& pipelines = m_renderCache->GetPipelineCache();
	EXPECT_EQ(pipelines.GetPipeline(shaderId, version), nullptr);

	EXPECT_TRUE(m_geometryPass->EnsureMeshPipeline(*m_renderCache, *shader));
	EXPECT_NE(pipelines.GetPipeline(shaderId, version), nullptr);

	// The warmed pipeline is now the one Record binds; recording must succeed.
	PassStats stats = RecordFrame(scene);
	EXPECT_EQ(stats.drawCalls, 1u);
}

// ---------------------------------------------------------------------------
// 3. Keyed on the Shader, not the mesh. Two meshes share one Shader, so one
//    warm serves both and the entry lives under the shader id, never a mesh id.
// ---------------------------------------------------------------------------

TEST_F(GeometryPipelineWarmTest, PipelineKeyedOnShaderNotMesh)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("SharedShader");
	const int shaderId = shader->GetObjectID();
	const int version  = shader->GetVersion();

	auto meshA = MakeMesh(shader);
	auto meshB = MakeMesh(shader);
	Scene scene;
	BuildScene(scene, {meshA, meshB});

	EXPECT_TRUE(m_geometryPass->EnsureMeshPipeline(*m_renderCache, *shader));

	PipelineCache& pipelines = m_renderCache->GetPipelineCache();
	// Keyed on the shader id ...
	Pipeline* warmed = pipelines.GetPipeline(shaderId, version);
	EXPECT_NE(warmed, nullptr);
	// ... never on either mesh id. Keying on the mesh id (the bug the re-key
	// fixed) would leave Get(shaderId) null and populate a mesh id instead.
	EXPECT_EQ(pipelines.Get(meshA->GetObjectID()), nullptr);
	EXPECT_EQ(pipelines.Get(meshB->GetObjectID()), nullptr);

	// A second warm is a no-op: the same entry is returned, so no second compile
	// happened and the single entry serves both meshes.
	EXPECT_TRUE(m_geometryPass->EnsureMeshPipeline(*m_renderCache, *shader));
	EXPECT_EQ(pipelines.GetPipeline(shaderId, version), warmed);

	// Both meshes draw against that one warmed pipeline.
	PassStats stats = RecordFrame(scene);
	EXPECT_EQ(stats.drawCalls, 2u);
}

// ---------------------------------------------------------------------------
// 4. A version bump invalidates the version-aware lookup while the
//    version-agnostic one still finds the stale entry - the combination that
//    makes the next store "replacing" rather than "inserting".
// ---------------------------------------------------------------------------

TEST_F(GeometryPipelineWarmTest, VersionBumpInvalidatesLookup)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("VersionedShader");
	const int shaderId   = shader->GetObjectID();
	const int oldVersion = shader->GetVersion();

	EXPECT_TRUE(m_geometryPass->EnsureMeshPipeline(*m_renderCache, *shader));

	PipelineCache& pipelines = m_renderCache->GetPipelineCache();
	EXPECT_NE(pipelines.GetPipeline(shaderId, oldVersion), nullptr);

	shader->BumpVersion();
	const int newVersion = shader->GetVersion();
	ASSERT_NE(newVersion, oldVersion);

	// Version-aware lookup misses the stale entry ...
	EXPECT_EQ(pipelines.GetPipeline(shaderId, newVersion), nullptr);
	// ... but the entry is still there, version-agnostically - this is what
	// makes the re-warm's UsePipeline() replace rather than insert.
	EXPECT_NE(pipelines.Get(shaderId), nullptr);

	// Re-warm stores the new version; the version-aware lookup now hits.
	EXPECT_TRUE(m_geometryPass->EnsureMeshPipeline(*m_renderCache, *shader));
	EXPECT_NE(pipelines.GetPipeline(shaderId, newVersion), nullptr);
}

// ---------------------------------------------------------------------------
// 5. No usable SPIR-V is an ordinary edit-time state, not an error:
//    EnsureMeshPipeline returns false, caches nothing, and Record draws the
//    mesh with the pass default.
// ---------------------------------------------------------------------------

TEST_F(GeometryPipelineWarmTest, NoUsableSpirv_WarmReturnsFalse_CacheStaysEmpty)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("UncompiledShader", /*compile=*/false);
	const int shaderId = shader->GetObjectID();
	const int version  = shader->GetVersion();

	Scene scene;
	BuildScene(scene, {MakeMesh(shader)});

	EXPECT_FALSE(m_geometryPass->EnsureMeshPipeline(*m_renderCache, *shader));

	PipelineCache& pipelines = m_renderCache->GetPipelineCache();
	// Nothing cached: a false warm must not store an empty pipeline.
	EXPECT_EQ(pipelines.GetPipeline(shaderId, version), nullptr);
	EXPECT_EQ(pipelines.Get(shaderId), nullptr);

	// The mesh still draws, with the pass default.
	PassStats stats = RecordFrame(scene);
	EXPECT_EQ(stats.drawCalls, 1u);
}
