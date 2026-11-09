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
#include "render/requests/RenderRequestExecutor.h"
#include "render/requests/RenderRequests.h"
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
#include <functional>
#include <memory>
#include <optional>
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
				[this]
				{
					++m_drains;
					// A test can observe the cache at the exact moment a handler
					// asks for the frames to be waited, before its own work runs.
					if (m_onDrain) m_onDrain();
					m_device->waitIdle();
				},
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

	/// @brief Runs inside the drain, before the requesting handler's own work, so
	///        a test can read the cache at the moment the frames are waited.
	std::function<void()> m_onDrain;
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

	const MeshGPU* resident = m_cache->GetMeshGPU(kObjectId);
	ASSERT_NE(resident, nullptr);
	EXPECT_NE(resident->vertexBuffer, nullptr);
	EXPECT_NE(resident->indexBuffer, nullptr);
	EXPECT_EQ(resident->indexCount, 3u);

	// Posting twice is harmless by design: the second request sees residency and
	// returns without touching the table. Compare the owned buffer, not the
	// MeshGPU address: a re-upload would install a new table with a fresh entry,
	// so only the buffer proves the slot was left alone.
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
	const EnvironmentGPU* env = m_cache->GetEnvironmentGPU(kEnvId);
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
	const EnvironmentGPU* first = m_cache->GetEnvironmentGPU(kEnvId);
	ASSERT_NE(first, nullptr);
	Texture* firstDiffuse = first->diffuseTexture.get();
	ASSERT_NE(firstDiffuse, nullptr);

	// regenerate=false over a resident entry: skipped. The same diffuse texture
	// is still mapped - a handler that regenerated anyway would swap the pointer.
	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = false});
	const EnvironmentGPU* skipped = m_cache->GetEnvironmentGPU(kEnvId);
	ASSERT_NE(skipped, nullptr);
	EXPECT_EQ(skipped->diffuseTexture.get(), firstDiffuse);

	// regenerate=true: the cached cubemaps are exactly what went stale, so the
	// entry is replaced and the diffuse texture pointer must change.
	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = true});
	const EnvironmentGPU* regenerated = m_cache->GetEnvironmentGPU(kEnvId);
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
// 9b. More shadow-casting lights than MAX_SHADOW_LAYERS: the rebuild assigns one
//     layer each to the first four and leaves the fifth at -1. The layer lookup
//     must report that as absence instead of asserting or handing out layer 0 -
//     layer 0 belongs to another light, and exporting it under the fifth light's
//     name would silently mislabel a real shadow map.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, ShadowLayerOverflow_ExtraLightGetsNoLayer)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	UpdateLightingRequest rebuild;
	for (int uid = 1; uid <= 5; ++uid)
	{
		rebuild.lights.emplace(uid, PointLightStruct{});
	}
	m_executor->Execute(std::move(rebuild));

	// Layers are assigned in UID order, so uids 1..4 own 0..3 and uid 5 has none.
	for (int uid = 1; uid <= 4; ++uid)
	{
		const std::optional<uint32_t> layer = m_cache->GetShadowIntensityLayerIndex(uid);
		ASSERT_TRUE(layer.has_value()) << "Light uid " << uid << " lost its layer";
		EXPECT_EQ(*layer, static_cast<uint32_t>(uid - 1));
	}
	EXPECT_FALSE(m_cache->GetShadowIntensityLayerIndex(5).has_value())
		<< "Light uid 5 is past MAX_SHADOW_LAYERS and must have no layer";

	// The allocating lookup has the same answer and must not grow the map. The
	// extent is unused by the lookup; any value works.
	const vk::Extent2D lookupExtent(256, 256);
	EXPECT_FALSE(m_cache->GetShadowIntensityLayer(5, lookupExtent).has_value());
	EXPECT_FALSE(m_cache->GetShadowIntensityLayerIndex(5).has_value())
		<< "A lookup for an overflow light must not allocate a layer";
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
// 14. Regenerating over a resident environment replaces its table entry, which
//     destroys nothing the GPU is reading: the frames already submitted hold the
//     old table, so the handler must NOT drain the frames.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UploadEnvironment_RegenerateResident_DoesNotDrain)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kEnvId = 401;
	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = false});
	const EnvironmentGPU* first = m_cache->GetEnvironmentGPU(kEnvId);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(first->diffuseTexture, nullptr);
	// The cubemaps the regenerate is about to displace.
	const Texture* firstDiffuse = first->diffuseTexture.get();
	ASSERT_EQ(m_drains, 0u);

	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = true});

	const EnvironmentGPU* regenerated = m_cache->GetEnvironmentGPU(kEnvId);
	ASSERT_NE(regenerated, nullptr);
	EXPECT_NE(regenerated->diffuseTexture.get(), firstDiffuse);
	// Nothing was destroyed - the displaced entry lives in the held table - so
	// waiting the frames would have been pure cost.
	EXPECT_EQ(m_drains, 0u);
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

// ===========================================================================
// UpdateLightingRequest - light SSBO growth
// ===========================================================================
//
// ArrayBuffer::Upload resizes only when a count exceeds the capacity, and
// Resize is what destroys the old GPUBuffer a submitted frame may still be
// reading. A first allocation and an in-place rewrite free nothing, so only
// growth may cost a drain - and the drain has to come BEFORE the resize.

namespace
{
	/// @brief A rebuild request carrying one default point light per UID.
	UpdateLightingRequest MakePointLightRequest(const std::vector<int>& uids)
	{
		UpdateLightingRequest request;
		for (const int uid : uids)
			request.lights.emplace(uid, PointLightStruct{});
		return request;
	}

	/// @brief A rebuild request carrying one default sun light per UID.
	UpdateLightingRequest MakeSunLightRequest(const std::vector<int>& uids)
	{
		UpdateLightingRequest request;
		for (const int uid : uids)
			request.lights.emplace(uid, SunLightStruct{});
		return request;
	}

	/// @brief A rebuild request carrying one default spot light per UID.
	UpdateLightingRequest MakeSpotLightRequest(const std::vector<int>& uids)
	{
		UpdateLightingRequest request;
		for (const int uid : uids)
			request.lights.emplace(uid, SpotLightStruct{});
		return request;
	}
}

// ---------------------------------------------------------------------------
// 18. The first light allocation creates the SSBO and destroys nothing, so the
//     request must not wait the frames. The capacity proves a buffer now exists
//     at exactly the size of the uploaded set.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UpdateLighting_FirstAllocation_DoesNotDrain)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	LightingCache* lighting = m_cache->GetLightingCache();
	ASSERT_NE(lighting, nullptr);
	// Lazily allocated: no buffer before the first upload, so there is nothing
	// a first allocation could replace.
	ASSERT_EQ(lighting->GetPointLightCapacity(), 0u);
	ASSERT_EQ(lighting->GetPointLightSSBO(), nullptr);

	m_executor->Execute(MakePointLightRequest({1}));

	EXPECT_NE(lighting->GetPointLightSSBO(), nullptr);
	EXPECT_EQ(lighting->GetPointLightCapacity(), 1u);
	EXPECT_EQ(lighting->GetPointLightCount(), 1u);
	// Created, not replaced.
	EXPECT_EQ(m_drains, 0u);
}

// ---------------------------------------------------------------------------
// 19. An equal or smaller light set rewrites the existing allocation in place;
//     the buffer is never resized, so no frame may be reading a freed one and
//     no drain is warranted. Shrinking keeps the capacity, so a later count up
//     to it still fits - which is what the last upload of zero lights exercises.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UpdateLighting_SameOrFewer_DoesNotDrain)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	LightingCache* lighting = m_cache->GetLightingCache();
	ASSERT_NE(lighting, nullptr);

	m_executor->Execute(MakePointLightRequest({1, 2}));
	ASSERT_EQ(lighting->GetPointLightCapacity(), 2u);
	ASSERT_EQ(m_drains, 0u);

	m_executor->Execute(MakePointLightRequest({1, 2}));   // same count
	m_executor->Execute(MakePointLightRequest({1}));      // fewer
	m_executor->Execute(MakePointLightRequest({}));       // none

	EXPECT_EQ(m_drains, 0u);
	// The allocation survived the shrink: Upload only resizes on growth.
	EXPECT_EQ(lighting->GetPointLightCapacity(), 2u);
	EXPECT_EQ(lighting->GetPointLightCount(), 0u);
}

// ---------------------------------------------------------------------------
// 20. Growing past the capacity replaces the buffer, so the handler must wait
//     the in-flight frames - and the wait must land before the resize: at the
//     moment of the drain the old buffer is still the live one.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UpdateLighting_Growth_DrainsOnceBeforeTheResize)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	LightingCache* lighting = m_cache->GetLightingCache();
	ASSERT_NE(lighting, nullptr);

	m_executor->Execute(MakePointLightRequest({1}));
	ASSERT_EQ(m_drains, 0u);

	// The buffer one light is reading: it must still be live at drain time and
	// gone right after.
	const GPUBuffer* before = lighting->GetPointLightSSBO();
	ASSERT_NE(before, nullptr);

	bool drainedBeforeResize = false;
	m_onDrain = [&]
	{
		drainedBeforeResize = lighting->GetPointLightSSBO() == before;
	};

	m_executor->Execute(MakePointLightRequest({1, 2, 3}));

	EXPECT_EQ(m_drains, 1u);
	// The frame wait happened while the old buffer was still the live one; a
	// handler that uploaded first and drained after would see the new buffer.
	EXPECT_TRUE(drainedBeforeResize);
	// The growth really did replace the allocation, which is why it had to wait.
	EXPECT_NE(lighting->GetPointLightSSBO(), nullptr);
	EXPECT_NE(lighting->GetPointLightSSBO(), before);
	EXPECT_EQ(lighting->GetPointLightCapacity(), 3u);
}

// ---------------------------------------------------------------------------
// 21. Growth of any kind drains, not just point lights: the three SSBOs are
//     separate allocations, so each kind's first growth past its capacity is
//     its own replacement. A kind's first upload is still not growth.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, UpdateLighting_GrowthInAnyKind_Drains)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	LightingCache* lighting = m_cache->GetLightingCache();
	ASSERT_NE(lighting, nullptr);

	// Sun lights: first upload allocates, the second outgrows it.
	m_executor->Execute(MakeSunLightRequest({1}));
	ASSERT_EQ(m_drains, 0u);
	m_executor->Execute(MakeSunLightRequest({1, 2}));
	ASSERT_EQ(m_drains, 1u);

	// Spot lights repeat it on their own allocation.
	m_executor->Execute(MakeSpotLightRequest({1}));
	ASSERT_EQ(m_drains, 1u);
	m_executor->Execute(MakeSpotLightRequest({1, 2}));
	EXPECT_EQ(m_drains, 2u);

	// Each kind's SSBO is its own: the sun and spot growth left the never-used
	// point SSBO unallocated.
	EXPECT_EQ(lighting->GetPointLightCapacity(), 0u);
	EXPECT_EQ(lighting->GetSunLightCapacity(), 2u);
	EXPECT_EQ(lighting->GetSpotLightCapacity(), 2u);
}

// ===========================================================================
// Held tables - what replaces the drain for refcounted resources
// ===========================================================================

// ---------------------------------------------------------------------------
// 22. A regenerated environment is NOT drained: the table a submitted frame
//     holds keeps the displaced cubemaps alive on its own, until that table is
//     released. This is the observable difference between draining and holding -
//     the old entry outlives its replacement in the cache.
// ---------------------------------------------------------------------------

TEST_F(RenderRequestExecutorTest, HeldEnvironmentTable_KeepsTheReplacedEnvironmentAlive)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kEnvId = 403;
	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = false});
	ASSERT_NE(m_cache->GetEnvironmentGPU(kEnvId), nullptr);

	// What a submitted frame holds, plus a way to observe the entry itself.
	std::shared_ptr<const EnvironmentTable> held = m_cache->Environments();
	const auto it = held->find(kEnvId);
	ASSERT_NE(it, held->end());
	std::weak_ptr<const EnvironmentGPU> entry = it->second;
	const Texture* heldDiffuse = it->second->diffuseTexture.get();
	ASSERT_NE(heldDiffuse, nullptr);

	m_executor->Execute(UploadEnvironmentRequest{
		.objectId = kEnvId, .equirect = MakeEquirect(), .regenerate = true});

	// The cache shows the regenerated cubemaps ...
	const EnvironmentGPU* regenerated = m_cache->GetEnvironmentGPU(kEnvId);
	ASSERT_NE(regenerated, nullptr);
	EXPECT_NE(regenerated->diffuseTexture.get(), heldDiffuse);
	// ... while the replaced entry is still alive for the frame drawing it.
	EXPECT_FALSE(entry.expired());

	held.reset();
	EXPECT_TRUE(entry.expired());
}
