/**
 * @file test_render_request_processor.cpp
 * @brief Offscreen coverage for RenderRequestProcessor - the drain order and the
 *        split between renderer-state requests and GPU requests.
 *
 * The executor tests prove each GPU handler; this file proves what surrounds
 * them: `RedrawOverlaysRequest` marks the overlays without restarting
 * accumulation, `ResetAccumulationRequest` restarts and stops, and every GPU
 * request restarts accumulation BEFORE its handler runs. The FIFO case pins the
 * order across kinds in one batch.
 *
 * @note Requires a Vulkan 1.4-capable GPU. Skipped in CI without GPU.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/RenderCache.h"
#include "render/requests/RenderRequestProcessor.h"
#include "render/requests/RenderRequests.h"
#include "render/UploadManager.h"
#include "render/passes/GeometryPass.h"
#include "render/resources/LightingCache.h"
#include "render/resources/MeshGPU.h"

#include "asset/data/MeshData.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace neurus;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

/**
 * @brief Wires a processor over the same offscreen collaborators the renderer
 *        hands it, and records which state callbacks ran, in order.
 */
class RenderRequestProcessorTest : public VulkanTestShared
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
			m_processor = std::make_unique<RenderRequestProcessor>(
				[this]
				{
					++m_drains;
					m_device->waitIdle();
				},
				*m_cache, m_geometryPass.get(),
				[this]
				{
					m_events.push_back("restart");
					if (m_onRestart) m_onRestart();
				},
				[this] { m_events.push_back("overlays"); });

			// The Application wires this in after the renderer exists; the
			// processor starts without one for the same reason.
			m_processor->SetUploadManager(m_upload.get());
		}
		catch (const std::exception& e)
		{
			std::cerr << "[RenderRequestProcessorTest::SetUp] " << e.what() << std::endl;
			m_hasVulkan = false;
		}

		m_drains = 0;
	}

	void TearDown() override
	{
		if (m_device) m_device->waitIdle();
		m_processor.reset();
		m_geometryPass.reset();
		m_upload.reset();
		m_cache.reset();
		VulkanTestShared::TearDown();
	}

	/// @brief A single-triangle MeshData, the smallest valid upload payload.
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

	std::unique_ptr<RenderCache>           m_cache;
	std::unique_ptr<UploadManager>         m_upload;
	std::unique_ptr<GeometryPass>          m_geometryPass;
	std::unique_ptr<RenderRequestProcessor> m_processor;

	std::vector<std::string> m_events;  ///< Ordered state callbacks seen by Drain.
	uint32_t                 m_drains = 0;

	/// @brief Runs inside the restart callback, before any handler's work.
	std::function<void()> m_onRestart;
};

// ---------------------------------------------------------------------------
// 1. Overlay-only redraw never restarts accumulation
// ---------------------------------------------------------------------------

TEST_F(RenderRequestProcessorTest, RedrawOverlays_MarksOverlaysWithoutRestarting)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	m_processor->Post(RedrawOverlaysRequest{});
	m_processor->Drain();

	ASSERT_EQ(m_events.size(), 1u);
	EXPECT_EQ(m_events[0], "overlays");
	EXPECT_EQ(m_drains, 0u);
}

// ---------------------------------------------------------------------------
// 2. Reset restarts accumulation and stops before the executor
// ---------------------------------------------------------------------------

TEST_F(RenderRequestProcessorTest, ResetAccumulation_RestartsAndStops)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kObjectId = 401;
	m_processor->Post(ResetAccumulationRequest{});
	m_processor->Drain();

	ASSERT_EQ(m_events.size(), 1u);
	EXPECT_EQ(m_events[0], "restart");
	EXPECT_EQ(m_drains, 0u);

	// The reset is not a GPU request: the cache stayed empty. If the processor
	// had forwarded it to the executor, this would still be empty (it has no
	// handler now), so the compile-time constraint is the real guard; this
	// assertion pins the "nothing happened" half.
	EXPECT_EQ(m_cache->GetMeshGPU(kObjectId), nullptr);
}

// ---------------------------------------------------------------------------
// 3. A GPU request restarts accumulation BEFORE its handler runs
// ---------------------------------------------------------------------------

TEST_F(RenderRequestProcessorTest, GpuRequest_RestartsBeforeTheHandlerRuns)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kObjectId = 402;
	bool hadMeshAtRestart = true;
	m_onRestart = [this, &hadMeshAtRestart]
	{
		hadMeshAtRestart = (m_cache->GetMeshGPU(kObjectId) != nullptr);
	};

	m_processor->Post(UploadMeshRequest{.objectId = kObjectId, .data = MakeTriangle()});
	m_processor->Drain();

	EXPECT_FALSE(hadMeshAtRestart) << "accumulation must restart before the upload";
	EXPECT_NE(m_cache->GetMeshGPU(kObjectId), nullptr) << "the upload must still run";

	ASSERT_EQ(m_events.size(), 1u);
	EXPECT_EQ(m_events[0], "restart");
}

// ---------------------------------------------------------------------------
// 4. One batch keeps FIFO order across kinds and across the state/GPU split
// ---------------------------------------------------------------------------

TEST_F(RenderRequestProcessorTest, Drain_AppliesFifoAcrossStateAndGpuKinds)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	constexpr int kObjectId = 403;

	// overlays (no restart), upload (restart + handler), drop (restart + drain),
	// reset (restart only).
	m_processor->Post(RedrawOverlaysRequest{});
	m_processor->Post(UploadMeshRequest{.objectId = kObjectId, .data = MakeTriangle()});
	m_processor->Post(DropSceneResourcesRequest{});
	m_processor->Post(ResetAccumulationRequest{});
	m_processor->Drain();

	const std::vector<std::string> expected = { "overlays", "restart", "restart", "restart" };
	EXPECT_EQ(m_events, expected);

	// The drop handler is the only request that waits the in-flight frames, so
	// one drain proves the FIFO order reached the executor after the upload.
	EXPECT_EQ(m_drains, 1u);
	EXPECT_EQ(m_cache->GetMeshGPU(kObjectId), nullptr);
}

// ---------------------------------------------------------------------------
// 5. A request posted during the drain lands in the next batch
// ---------------------------------------------------------------------------

TEST_F(RenderRequestProcessorTest, PostDuringDrain_LandsInTheNextBatch)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	// The restart callback stands in for a handler that posts follow-up work.
	m_onRestart = [this] { m_processor->Post(ResetAccumulationRequest{}); };

	m_processor->Post(UploadMeshRequest{.objectId = 404, .data = MakeTriangle()});
	m_processor->Drain();

	// Only the outer request ran: the nested one is still pending.
	ASSERT_EQ(m_events.size(), 1u);
	EXPECT_EQ(m_events[0], "restart");

	m_onRestart = nullptr;
	m_processor->Drain();
	ASSERT_EQ(m_events.size(), 2u);
	EXPECT_EQ(m_events[1], "restart");
}
