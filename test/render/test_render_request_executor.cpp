/**
 * @file test_render_request_executor.cpp
 * @brief Offscreen GPU coverage for RenderRequestExecutor - the handlers that
 *        used to live in the Editor, now proven against a real device.
 *
 * DeferredRenderer cannot be built headless (it needs a window surface), so its
 * request handling could never be exercised offscreen. The executor is the seam
 * that fixes that: it needs only a device, a RenderCache, an UploadManager and a
 * GeometryPass - all of which this fixture builds offscreen - and
 * DeferredRenderer merely delegates to it. Every test here drives a request the
 * way the per-frame drain does and asserts the observable GPU-side effect.
 *
 * Each assertion names the effect it depends on: residency flipping from absent
 * to present, FIFO ordering surviving a scene drop mid-batch, a sun light
 * getting a depth map and no colour cubemap where a point light gets both, a
 * regenerate replacing the cubemap texture it found vs an idempotent upload
 * leaving it, a pipeline request carrying its own SPIR-V so a recompile cannot
 * retroactively change which version it builds, and a single-light SSBO rewrite
 * landing on the uid an earlier full rebuild indexed.
 *
 * @note Requires a Vulkan 1.4-capable GPU. Skipped in CI without GPU.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/RenderCache.h"
#include "render/RenderRequestExecutor.h"
#include "render/RenderRequests.h"
#include "render/UploadManager.h"
#include "render/passes/GeometryPass.h"
#include "render/resources/EnvironmentGPU.h"
#include "render/resources/LightGPU.h"
#include "render/resources/LightingCache.h"
#include "render/resources/MeshGPU.h"
#include "render/resources/PipelineCache.h"
#include "render/shaders/RenderShader.h"

#include "asset/data/ImageData.h"
#include "asset/PixelFormat.h"
#include "asset/data/MeshData.h"
#include "scene/Light.h"

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

using namespace neurus;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

/**
 * @brief Offscreen GPU fixture wiring an executor over a real device.
 *
 * Builds the exact set of collaborators DeferredRenderer hands the executor -
 * RenderCache (with LightingCache), UploadManager, GeometryPass - and the
 * executor itself, so a request can be run headless and its effect read back
 * through the same public surfaces production uses.
 */
class RenderRequestExecutorTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;

		try
		{
			auto& pd = PhysicalDevice();
			m_cache = std::make_unique<RenderCache>(*m_device, pd);
			m_cache->SetLightingCache(std::make_unique<LightingCache>(
				*m_device, pd, *m_queue, m_graphicsQueueFamily));
			m_upload = std::make_unique<UploadManager>(
				*m_device, pd, *m_queue, m_graphicsQueueFamily);
			m_geometryPass = std::make_unique<GeometryPass>(*m_device, pd);
			m_executor = std::make_unique<RenderRequestExecutor>(
				[this] { ++m_drains; m_device->waitIdle(); },
				*m_cache, m_upload.get(), m_geometryPass.get());
		}
		catch (const std::exception& e)
		{
			std::cerr << "[RenderRequestExecutorTest::SetUp] " << e.what() << std::endl;
			m_hasVulkan = false;
		}

		// Count only what a test's own requests asked for: wiring the executor
		// must not have drained.
		m_drains = 0;
	}

	void TearDown() override
	{
		if (m_device) m_device->waitIdle();
		m_executor.reset();
		m_geometryPass.reset();
		m_upload.reset();
		m_cache.reset();
		VulkanTestShared::TearDown();
	}

	// A single-triangle MeshData, the smallest valid geometry an upload accepts.
	std::shared_ptr<MeshData> MakeTriangle()
	{
		auto data = std::make_shared<MeshData>();
		data->LoadObjFromString(
			"v 0.0 0.0 0.0\n"
			"v 1.0 0.0 0.0\n"
			"v 0.0 1.0 0.0\n"
			"vn 0.0 0.0 1.0\n"
			"f 1//1 2//1 3//1\n");
		return data;
	}

	// A compiled gbuffer RenderShader - the same real device-free compile the
	// Editor's create-shader flow runs. Mirrors GeometryPipelineWarmTest.
	std::shared_ptr<RenderShader> MakeShader(const char* name)
	{
		auto shader = std::make_shared<RenderShader>(
			name,
			ResolveAssetPath("res/shaders/render/gbuffer.vert"),
			ResolveAssetPath("res/shaders/render/gbuffer.frag"));
		shader->ParseAndGenerate();
		shader->CompileToSpv();
		return shader;
	}

	std::unique_ptr<RenderCache>           m_cache;
	std::unique_ptr<UploadManager>         m_upload;
	std::unique_ptr<GeometryPass>          m_geometryPass;
	std::unique_ptr<RenderRequestExecutor> m_executor;

	/// @brief How many times this test's requests asked for the frames to be
	///        drained; reset once the fixture is wired, so only requests count.
	uint32_t m_drains = 0;
};

// ===========================================================================
// UploadMeshRequest
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. Not resident before, resident after; a second identical request is a
//    harmless no-op leaving the same slot.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UploadMesh_FlipsResidency_SecondIsNoOp)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kObjectId = 1;
	// Absent before: the slot is empty, so the assertion can fail if a prior
	// test leaked it or the executor uploaded nothing.
	ASSERT_EQ(m_cache->GetMeshGPU(kObjectId), nullptr);

	m_executor->Execute(UploadMeshRequest{.objectId = kObjectId, .data = MakeTriangle()});

	MeshGPU* resident = m_cache->GetMeshGPU(kObjectId);
	ASSERT_NE(resident, nullptr);
	EXPECT_NE(resident->vertexBuffer, nullptr);
	EXPECT_NE(resident->indexBuffer, nullptr);
	EXPECT_EQ(resident->indexCount, 3u);

	// Posting twice is harmless by design: the second request sees residency and
	// returns without touching the slot. Compare the owned buffer, not the
	// MeshGPU address: a re-upload assigned into the same map slot would keep
	// that address while replacing the buffer, so only the buffer can tell.
	const auto* firstVertexBuffer = resident->vertexBuffer.get();
	m_executor->Execute(UploadMeshRequest{.objectId = kObjectId, .data = MakeTriangle()});
	ASSERT_NE(m_cache->GetMeshGPU(kObjectId), nullptr);
	EXPECT_EQ(m_cache->GetMeshGPU(kObjectId)->vertexBuffer.get(), firstVertexBuffer);
}

// ---------------------------------------------------------------------------
// 2. Null MeshData is a named error that uploads nothing - never an empty slot.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UploadMesh_NullData_UploadsNothing)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kObjectId = 2;
	m_executor->Execute(UploadMeshRequest{.objectId = kObjectId, .data = nullptr});

	// The handler skipped: no slot was created. A handler that uploaded an empty
	// slot anyway would make this non-null and fail.
	EXPECT_EQ(m_cache->GetMeshGPU(kObjectId), nullptr);
}

// ---------------------------------------------------------------------------
// 3. FIFO through the executor: [uploadA, DropScene, uploadB] leaves only B
//    resident. Executing in order drops A with the scene, then B lands after.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, FifoOrder_DropSceneBetweenUploads)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kIdA = 10;
	constexpr int kIdB = 11;

	// Executed in FIFO order, the way the per-frame drain walks a batch.
	m_executor->Execute(UploadMeshRequest{.objectId = kIdA, .data = MakeTriangle()});
	// A is resident at this point ...
	ASSERT_NE(m_cache->GetMeshGPU(kIdA), nullptr);

	m_executor->Execute(DropSceneResourcesRequest{});
	m_executor->Execute(UploadMeshRequest{.objectId = kIdB, .data = MakeTriangle()});

	// ... but the drop released it, and B - posted after the drop - survives.
	// Reversing execution order (B before drop) would flip both assertions.
	EXPECT_EQ(m_cache->GetMeshGPU(kIdA), nullptr);
	EXPECT_NE(m_cache->GetMeshGPU(kIdB), nullptr);
}

// ===========================================================================
// CreateLightGpuRequest
// ===========================================================================

// ---------------------------------------------------------------------------
// 4. A sun light gets a depth map and no colour cubemap; a point light gets
//    both. The shadow-map shape is what the light type decides, and it is the
//    one observable difference between the two allocations.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, CreateLightGpu_SunDepthOnly_PointBoth)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kSunUid   = 100;
	constexpr int kPointUid = 101;
	ASSERT_EQ(m_cache->GetLightGPU(kSunUid), nullptr);
	ASSERT_EQ(m_cache->GetLightGPU(kPointUid), nullptr);

	m_executor->Execute(CreateLightGpuRequest{.uid = kSunUid, .type = LightType::SUNLIGHT});
	m_executor->Execute(CreateLightGpuRequest{.uid = kPointUid, .type = LightType::POINTLIGHT});

	LightGPU* sun = m_cache->GetLightGPU(kSunUid);
	ASSERT_NE(sun, nullptr);
	// A directional sun renders into a single 2D depth map - depth, no colour.
	EXPECT_NE(sun->shadowDepthMap, nullptr);
	EXPECT_EQ(sun->shadowColorMap, nullptr);

	LightGPU* point = m_cache->GetLightGPU(kPointUid);
	ASSERT_NE(point, nullptr);
	// A point light renders a cube: depth cubemap plus a colour cubemap for the
	// multiview distance encoding. Both present is the type's signature.
	EXPECT_NE(point->shadowDepthMap, nullptr);
	EXPECT_NE(point->shadowColorMap, nullptr);
}

// ---------------------------------------------------------------------------
// 5. Already-resident light is a no-op: a second request leaves the same maps.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, CreateLightGpu_SecondIsNoOp)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kUid = 102;
	m_executor->Execute(CreateLightGpuRequest{.uid = kUid, .type = LightType::SUNLIGHT});

	LightGPU* first = m_cache->GetLightGPU(kUid);
	ASSERT_NE(first, nullptr);
	Image* firstDepth = first->shadowDepthMap.get();

	// Resident: the second request returns without reallocating, so the SAME
	// depth image is still mapped. A handler that rebuilt would swap the pointer.
	m_executor->Execute(CreateLightGpuRequest{.uid = kUid, .type = LightType::SUNLIGHT});
	LightGPU* second = m_cache->GetLightGPU(kUid);
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(second->shadowDepthMap.get(), firstDepth);
}

// ===========================================================================
// UploadEnvironmentRequest
// ===========================================================================

namespace
{
	// A tiny valid equirect: 16x8 RGBA32F, the float HDR layout IBL convolution
	// reads. Contents are irrelevant to the residency/replace assertions.
	std::shared_ptr<ImageData> MakeEquirect()
	{
		constexpr uint32_t w = 16, h = 8;
		std::vector<float> pixels(w * h * 4, 0.5f);
		return std::make_shared<ImageData>(
			pixels.data(), w, h, PixelFormat::RGBA32F);
	}
}

// ---------------------------------------------------------------------------
// 6. A null equirect selects the fallback and still produces a resident
//    environment - the same behaviour a missing source file had.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UploadEnvironment_NullEquirect_Fallback)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kEnvId = 200;
	ASSERT_EQ(m_cache->GetEnvironmentGPU(kEnvId), nullptr);

	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = nullptr, .regenerate = false});

	// Fallback still yields real cubemaps, not an empty entry.
	EnvironmentGPU* env = m_cache->GetEnvironmentGPU(kEnvId);
	ASSERT_NE(env, nullptr);
	EXPECT_NE(env->diffuseTexture, nullptr);
	EXPECT_NE(env->specularTexture, nullptr);
}

// ---------------------------------------------------------------------------
// 7. Resident + regenerate=false is the idempotent scene-upload path: the
//    expensive cubemap regeneration is SKIPPED, so the same texture survives.
//    Resident + regenerate=true REPLACES it with a freshly convolved texture.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UploadEnvironment_IdempotentVsRegenerate)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kEnvId = 201;

	// First upload creates the entry.
	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = false});
	EnvironmentGPU* first = m_cache->GetEnvironmentGPU(kEnvId);
	ASSERT_NE(first, nullptr);
	Texture* firstDiffuse = first->diffuseTexture.get();
	ASSERT_NE(firstDiffuse, nullptr);

	// regenerate=false over a resident entry: skipped. The same diffuse texture
	// is still mapped - a handler that regenerated anyway would swap the pointer.
	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = false});
	EnvironmentGPU* skipped = m_cache->GetEnvironmentGPU(kEnvId);
	ASSERT_NE(skipped, nullptr);
	EXPECT_EQ(skipped->diffuseTexture.get(), firstDiffuse);

	// regenerate=true: the cached cubemaps are exactly what went stale, so the
	// entry is replaced and the diffuse texture pointer must change.
	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = true});
	EnvironmentGPU* regenerated = m_cache->GetEnvironmentGPU(kEnvId);
	ASSERT_NE(regenerated, nullptr);
	EXPECT_NE(regenerated->diffuseTexture.get(), firstDiffuse);
}

// ===========================================================================
// EnsurePipelineRequest
// ===========================================================================

// ---------------------------------------------------------------------------
// 8. The request carries its OWN SPIR-V snapshot, so a recompile landing before
//    the drain cannot change which version it builds. Make the request at the
//    old version, bump + recompile the live Shader, then execute: the pipeline
//    built is keyed to the OLD version, never the new one.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, EnsurePipeline_BuildsSnapshottedVersion)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("SnapshotShader");
	const int shaderId   = shader->GetObjectID();
	const int oldVersion = shader->GetVersion();

	// Snapshot the shader into a request at the old version - this copies the
	// SPIR-V and version out of the live Shader.
	EnsurePipelineRequest request = MakeEnsurePipelineRequest(*shader);
	EXPECT_EQ(request.shaderId, shaderId);
	EXPECT_EQ(request.version, oldVersion);

	// The live Shader moves on: a recompile bumps the version under the editor's
	// hand while our request still holds the old SPIR-V.
	shader->BumpVersion();
	shader->CompileToSpv();
	const int newVersion = shader->GetVersion();
	ASSERT_NE(newVersion, oldVersion);

	m_executor->Execute(request);

	PipelineCache& pipelines = m_cache->GetPipelineCache();
	// Built at the OLD version the request captured ...
	EXPECT_NE(pipelines.GetPipeline(shaderId, oldVersion), nullptr);
	// ... never the new one the live Shader has since moved to. A handler that
	// read the live Shader instead of the request's snapshot would flip these.
	EXPECT_EQ(pipelines.GetPipeline(shaderId, newVersion), nullptr);
}

// ===========================================================================
// UpdateLightingRequest / UpdateLightRequest
// ===========================================================================

// ---------------------------------------------------------------------------
// 9. A full rebuild then a single-light rewrite: UpdateLighting populates the
//    uid->shadow-index map (observable via GetShadowIndex), and UpdateLight,
//    executed after it, finds a uid the rebuild indexed and leaves the index
//    intact. Two point lights so the second's shadow index is a non-zero value
//    distinct from the not-found sentinel.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UpdateLighting_ThenUpdateLight_FindsUid)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kUidA = 5;
	constexpr int kUidB = 6;

	// Not yet indexed: GetShadowIndex returns the 0 not-found sentinel.
	EXPECT_EQ(m_cache->GetShadowIndex(kUidB), 0u);

	UpdateLightingRequest rebuild;
	rebuild.lights.emplace(kUidA, PointLightStruct{});
	rebuild.lights.emplace(kUidB, PointLightStruct{});
	m_executor->Execute(std::move(rebuild));

	// Shadow indices are assigned in UID-sorted order: uid 5 -> 0, uid 6 -> 1.
	// The non-zero index for uid 6 proves the full rebuild ran and indexed it -
	// a skipped UpdateLighting would leave this at the 0 sentinel.
	EXPECT_EQ(m_cache->GetShadowIndex(kUidB), 1u);

	// UpdateLight for a uid the rebuild indexed is found and rewrites in place;
	// it must not disturb the shadow-index map the full rebuild built.
	m_executor->Execute(UpdateLightRequest{.uid = kUidB, .light = PointLightStruct{}});
	EXPECT_EQ(m_cache->GetShadowIndex(kUidB), 1u);

	// A single-light rewrite for a uid NEVER indexed is a documented no-op: it
	// adds no index, so GetShadowIndex stays at the not-found sentinel.
	constexpr int kUnknown = 999;
	m_executor->Execute(UpdateLightRequest{.uid = kUnknown, .light = PointLightStruct{}});
	EXPECT_EQ(m_cache->GetShadowIndex(kUnknown), 0u);
}

// ---------------------------------------------------------------------------
// 10. ResetAccumulationRequest is a no-op in the executor (DeferredRenderer owns
//     it): it must not crash and must leave the cache untouched. The no-op is
//     what keeps Execute's std::visit total, so handing it here is harmless.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, ResetAccumulation_IsHarmlessNoOp)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kObjectId = 300;
	m_executor->Execute(UploadMeshRequest{.objectId = kObjectId, .data = MakeTriangle()});
	MeshGPU* before = m_cache->GetMeshGPU(kObjectId);
	ASSERT_NE(before, nullptr);

	// Executing the reset does nothing to the executor's resources: the mesh is
	// still there, same slot. A handler that touched the cache would change this.
	m_executor->Execute(ResetAccumulationRequest{});
	EXPECT_EQ(m_cache->GetMeshGPU(kObjectId), before);
}

// ===========================================================================
// Frame drain - which handlers must wait the in-flight frames
// ===========================================================================
//
// Only a handler that REPLACES or FREES a resource a recorded frame may still
// read drains; a first insert and a handler whose work waits its own submit do
// not. Counting the drain (rather than timing it) is what makes that contract
// observable: a handler that idled the device anyway would still pass a
// residency assertion, but not a drain count.

// ---------------------------------------------------------------------------
// 11. A first pipeline insert displaces nothing, so it must not drain.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, EnsurePipeline_FirstInsert_DoesNotDrain)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("FirstInsertShader");
	ASSERT_EQ(m_cache->GetPipelineCache().Get(shader->GetObjectID()), nullptr);

	m_executor->Execute(MakeEnsurePipelineRequest(*shader));

	// The pipeline landed ...
	EXPECT_NE(m_cache->GetPipelineCache().GetPipeline(
		shader->GetObjectID(), shader->GetVersion()), nullptr);
	// ... without waiting the frames: nothing was destroyed to wait for.
	EXPECT_EQ(m_drains, 0u);
}

// ---------------------------------------------------------------------------
// 12. A second version under the same shader id replaces the resident pipeline,
//     destroying one a recorded frame may still bind: drain exactly once.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, EnsurePipeline_ReplaceVersion_DrainsOnce)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("ReplaceVersionShader");

	// The first version is an insert, not a replace, so it leaves the count at 0.
	m_executor->Execute(MakeEnsurePipelineRequest(*shader));
	ASSERT_EQ(m_drains, 0u);

	shader->BumpVersion();
	shader->CompileToSpv();
	const int newVersion = shader->GetVersion();

	m_executor->Execute(MakeEnsurePipelineRequest(*shader));

	EXPECT_NE(m_cache->GetPipelineCache().GetPipeline(
		shader->GetObjectID(), newVersion), nullptr);
	EXPECT_EQ(m_drains, 1u);
}

// ---------------------------------------------------------------------------
// 13. The first environment upload creates the entry and destroys nothing, so
//     it must not drain; its own upload wait covers the cubemaps it creates.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UploadEnvironment_First_DoesNotDrain)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kEnvId = 400;
	ASSERT_EQ(m_cache->GetEnvironmentGPU(kEnvId), nullptr);

	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = false});

	EXPECT_NE(m_cache->GetEnvironmentGPU(kEnvId), nullptr);
	EXPECT_EQ(m_drains, 0u);
}

// ---------------------------------------------------------------------------
// 14. Regenerating over a resident environment replaces its cubemaps, so the
//     handler must drain the frames once before destroying the old ones.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UploadEnvironment_RegenerateResident_DrainsOnce)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kEnvId = 401;
	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = false});
	ASSERT_NE(m_cache->GetEnvironmentGPU(kEnvId), nullptr);
	ASSERT_EQ(m_drains, 0u);

	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = true});

	EXPECT_EQ(m_drains, 1u);
}

// ---------------------------------------------------------------------------
// 15. Dropping the scene frees every cached entry a recorded frame may still
//     read, so it always drains - and the mesh upload before it, which waits its
//     own submit, does not.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, DropSceneResources_DrainsOnce)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kObjectId = 500;
	m_executor->Execute(UploadMeshRequest{.objectId = kObjectId, .data = MakeTriangle()});
	ASSERT_NE(m_cache->GetMeshGPU(kObjectId), nullptr);
	ASSERT_EQ(m_drains, 0u);

	m_executor->Execute(DropSceneResourcesRequest{});

	EXPECT_EQ(m_cache->GetMeshGPU(kObjectId), nullptr);
	EXPECT_EQ(m_drains, 1u);
}

// ---------------------------------------------------------------------------
// 16. regenerate=true for an id that is not resident is a first upload: there is
//     nothing to replace, so the drain keys on residency, not on the flag.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UploadEnvironment_RegenerateFresh_DoesNotDrain)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kEnvId = 402;
	ASSERT_EQ(m_cache->GetEnvironmentGPU(kEnvId), nullptr);

	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = true});

	EXPECT_NE(m_cache->GetEnvironmentGPU(kEnvId), nullptr);
	EXPECT_EQ(m_drains, 0u);
}

// ---------------------------------------------------------------------------
// 17. The same shader id at the same version is already resident: the store
//     keeps the entry, so the drain keys on the version, not on the id alone.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, EnsurePipeline_SameVersionTwice_DoesNotDrain)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	auto shader = MakeShader("SameVersionShader");

	m_executor->Execute(MakeEnsurePipelineRequest(*shader));
	m_executor->Execute(MakeEnsurePipelineRequest(*shader));

	EXPECT_NE(m_cache->GetPipelineCache().GetPipeline(
		shader->GetObjectID(), shader->GetVersion()), nullptr);
	EXPECT_EQ(m_drains, 0u);
}
