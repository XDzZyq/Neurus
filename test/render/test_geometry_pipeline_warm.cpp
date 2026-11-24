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

#include "shared/TestVulkanShared.hpp"

#include "render/RenderCache.hpp"
#include "render/UploadManager.hpp"
#include "render_api/RenderRequests.hpp"
#include "render_api/RenderContext.hpp"
#include "render/passes/GeometryPass.hpp"
#include "render/resources/LightingCache.hpp"
#include "render/resources/PipelineCache.hpp"
#include "render/resources/ShaderGPU.hpp"
#include "shader/RenderShader.hpp"
#include "shader/ShaderLibrary.hpp"
#include "shader/ShaderGenerator.hpp"

#include "asset/data/MeshData.hpp"
#include "scene/Camera.hpp"
#include "scene/Mesh.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
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
				*m_device, pd, *m_queue, m_graphicsQueueFamily));
			m_geometryPass = std::make_unique<GeometryPass>(*m_renderCache, *m_device, pd);
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
		auto shader = m_resources.Load<RenderShader>(
			name,
			ResolveAssetPath("res/shaders/render/gbuffer.vert.glsl"),
			ResolveAssetPath("res/shaders/render/gbuffer.frag.glsl"));
		if (compile)
		{
			shader->ParseAndGenerate();
			shader->CompileToSpv();
		}
		return shader;
	}

	bool UploadShader(const ShaderData& shader)
	{
		const auto resident = m_renderCache->GetShader(shader.RequireObjectID());
		if (resident && resident->GetVersion() == shader.GetVersion())
			return true;
		try
		{
			m_renderCache->UseShader(UploadManager::UploadShader(*m_device, MakeUploadShaderRequest(shader)));
			return true;
		}
		catch (const std::invalid_argument&)
		{
			return false;
		}
	}

	bool WarmShader(const ShaderData& shader)
	{
		if (!UploadShader(shader))
			return false;
		return m_geometryPass->EnsureMeshPipeline(*m_renderCache, MakeEnsurePipelineRequest(shader));
	}

	// A sphere mesh carrying @p shader as its per-mesh Shader.
	std::shared_ptr<Mesh> MakeMesh(std::shared_ptr<ShaderData> shader)
	{
		auto meshData = m_resources.Load<MeshData>();
		meshData->LoadObj(ResolveAssetPath("res/obj/sphere.obj"));

		auto mesh = m_resources.Load<Mesh>();
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
			*m_renderCache, scene, *m_device, PhysicalDevice(), *m_queue, m_graphicsQueueFamily);
		VulkanTestShared::TransitionGbufferToColorAttachment(
			*m_renderCache, {kWidth, kHeight}, *this);

		// `frame` must outlive `ctx`: declared first so it is destroyed last.
		// The mesh already carries its Shader (BuildScene attached it before we
		// got here), so the snapshot captures shaderObjectId/shaderVersion and
		// the custom-pipeline path still runs once Record() reads the snapshot.
		RenderFrameSnapshot frame;
		RenderContext ctx{
			.width = kWidth, .height = kHeight,
			.frameIndex = 0,
		};

		auto& cmd = BeginCmd();
		PublishFrame(*m_renderCache, ctx, frame, &scene);
		PassStats stats = RecordPass(*m_geometryPass, *cmd, *m_renderCache, ctx);
		EndSubmitWait(cmd);
		return stats;
	}

	// Registers an active camera and the given meshes into @p scene. Scene is a
	// UID (non-movable), so the caller owns it and we populate in place.
	void BuildScene(Scene& scene, std::initializer_list<std::shared_ptr<Mesh>> meshes)
	{
		auto camera = CreateTestCamera(kWidth, kHeight);
		scene.UseCamera(camera);
		scene.ActivateCamera(camera->RequireObjectID());
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
	const int shaderId = shader->RequireObjectID();
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
	const int shaderId = shader->RequireObjectID();
	const int version  = shader->GetVersion();

	Scene scene;
	BuildScene(scene, {MakeMesh(shader)});

	PipelineCache& pipelines = m_renderCache->GetPipelineCache();
	EXPECT_EQ(pipelines.GetPipeline(shaderId, version), nullptr);

	EXPECT_TRUE(WarmShader(*shader));
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
	const int shaderId = shader->RequireObjectID();
	const int version  = shader->GetVersion();

	auto meshA = MakeMesh(shader);
	auto meshB = MakeMesh(shader);
	Scene scene;
	BuildScene(scene, {meshA, meshB});

	EXPECT_TRUE(WarmShader(*shader));

	PipelineCache& pipelines = m_renderCache->GetPipelineCache();
	// Keyed on the shader id ...
	Pipeline* warmed = pipelines.GetPipeline(shaderId, version);
	EXPECT_NE(warmed, nullptr);
	// ... never on either mesh id. Keying on the mesh id (the bug the re-key
	// fixed) would leave Get(shaderId) null and populate a mesh id instead.
	EXPECT_EQ(pipelines.Get(meshA->RequireObjectID()), nullptr);
	EXPECT_EQ(pipelines.Get(meshB->RequireObjectID()), nullptr);

	// A second warm is a no-op: the same entry is returned, so no second compile
	// happened and the single entry serves both meshes.
	EXPECT_TRUE(WarmShader(*shader));
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
	const int shaderId   = shader->RequireObjectID();
	const int oldVersion = shader->GetVersion();

	EXPECT_TRUE(WarmShader(*shader));

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
	EXPECT_TRUE(WarmShader(*shader));
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
	const int shaderId = shader->RequireObjectID();
	const int version  = shader->GetVersion();

	Scene scene;
	BuildScene(scene, {MakeMesh(shader)});

	EXPECT_FALSE(UploadShader(*shader));
	EXPECT_EQ(m_renderCache->GetShader(shader->RequireObjectID()), nullptr);
	EXPECT_FALSE(m_geometryPass->EnsureMeshPipeline(*m_renderCache, MakeEnsurePipelineRequest(*shader)));

	PipelineCache& pipelines = m_renderCache->GetPipelineCache();
	// Nothing cached: a false warm must not store an empty pipeline.
	EXPECT_EQ(pipelines.GetPipeline(shaderId, version), nullptr);
	EXPECT_EQ(pipelines.Get(shaderId), nullptr);

	// The mesh still draws, with the pass default.
	PassStats stats = RecordFrame(scene);
	EXPECT_EQ(stats.drawCalls, 1u);
}

// ---------------------------------------------------------------------------
//    Compile lands one stage at a time: a fragment-only set is not a pipeline.
//    EnsureMeshPipeline returns false, caches nothing, and the next compile
//    that completes the pair warms normally.
// ---------------------------------------------------------------------------

TEST_F(GeometryPipelineWarmTest, FragmentOnlySpirv_DoesNotBuildPipeline)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("FragmentOnlyShader");
	shader->GetStage(ShaderType::VERTEX).spv.clear(); // mid-compile state
	shader->BumpVersion();
	const int shaderId = shader->RequireObjectID();
	const int version  = shader->GetVersion();

	Scene scene;
	BuildScene(scene, {MakeMesh(shader)});

	EXPECT_FALSE(UploadShader(*shader));
	EXPECT_EQ(m_renderCache->GetShader(shader->RequireObjectID()), nullptr);
	EXPECT_FALSE(m_geometryPass->EnsureMeshPipeline(*m_renderCache, MakeEnsurePipelineRequest(*shader)));

	PipelineCache& pipelines = m_renderCache->GetPipelineCache();
	EXPECT_EQ(pipelines.GetPipeline(shaderId, version), nullptr);
	EXPECT_EQ(pipelines.Get(shaderId), nullptr);

	PassStats stats = RecordFrame(scene);
	EXPECT_EQ(stats.drawCalls, 1u);
}
TEST_F(GeometryPipelineWarmTest, IncompatibleCompiledStagesUseDefaultWithoutCreatingPipeline)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("IncompatibleShader");
	ShaderUnit vertex;
	vertex.code = "#version 450\nvoid main() { gl_Position = vec4(0.0); }\n";
	shader->GetVertex().spv = ShaderLibrary::Compile(vertex, ShaderType::VERTEX, "MissingOutputs");
	ASSERT_FALSE(shader->GetVertex().spv.empty());
	ASSERT_FALSE(shader->GetFragment().spv.empty());
	shader->BumpVersion();

	EXPECT_FALSE(UploadShader(*shader));
	EXPECT_EQ(m_renderCache->GetShader(shader->RequireObjectID()), nullptr);
	EXPECT_FALSE(m_geometryPass->EnsureMeshPipeline(*m_renderCache, MakeEnsurePipelineRequest(*shader)));
	EXPECT_EQ(m_renderCache->GetPipelineCache().Get(shader->RequireObjectID()), nullptr);

	Scene scene;
	BuildScene(scene, {MakeMesh(shader)});
	EXPECT_EQ(RecordFrame(scene).drawCalls, 1u);
}

TEST_F(GeometryPipelineWarmTest, UploadedMeshShaderWithDifferentCameraBindingRejectsPipeline)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";
	auto shader = MakeShader("DifferentCameraBinding");
	auto& vertex = shader->GetVertex();
	auto camera = std::find_if(vertex.parsed.ubuffer_list.begin(), vertex.parsed.ubuffer_list.end(),
		[](const S_StructDef& block) { return block.name == "CameraUBO"; });
	ASSERT_NE(camera, vertex.parsed.ubuffer_list.end());
	camera->binding = 3;
	vertex.code = ShaderGenerator::Generate(vertex.parsed);
	vertex.spv.clear();
	ASSERT_TRUE(shader->CompileToSpv());

	// Vertex/fragment linkage remains valid, so upload accepts both modules.
	ASSERT_TRUE(UploadShader(*shader));
	ASSERT_TRUE(m_renderCache->GetShader(shader->RequireObjectID()));
	// Mesh drawing binds GeometryPass's fixed camera set; reject the changed
	// contract before pipeline creation instead of reaching Vulkan validation.
	EXPECT_FALSE(m_geometryPass->EnsureMeshPipeline(*m_renderCache, MakeEnsurePipelineRequest(*shader)));
	EXPECT_EQ(m_renderCache->GetPipelineCache().Get(shader->RequireObjectID()), nullptr);

	Scene scene;
	BuildScene(scene, {MakeMesh(shader)});
	EXPECT_EQ(RecordFrame(scene).drawCalls, 1u);
}
