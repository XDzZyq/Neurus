/**
 * @file test_render_graph_pipeline.cpp
 * @brief The production renderer compiles its scene graph from the document.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.hpp"

#include "core/RenderConfig.hpp"
#include "asset/data/render_graph/RenderGraphSpec.hpp"
#include "render/DeferredRenderer.hpp"
#include "render/SharedQueue.hpp"
#include "render/UploadManager.hpp"
#include "render/resources/LightingCache.hpp"
#include "scene/Camera.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <memory>

namespace
{

constexpr uint32_t kWidth  = 96;
constexpr uint32_t kHeight = 64;

} // anonymous namespace

class RenderGraphPipelineTest : public VulkanTestShared
{
protected:
	void TearDown() override
	{
		m_renderer.reset();
		m_uploader.reset();
		VulkanTestShared::TearDown();
	}

	void CreateRenderer()
	{
		m_renderer = std::make_unique<neurus::DeferredRenderer>(
			*m_device, m_physicalDevices[m_selectedPdIndex], *m_queue,
			m_graphicsQueueFamily, vk::Extent2D{kWidth, kHeight});

		m_uploader = std::make_unique<neurus::UploadManager>(
			*m_device, m_physicalDevices[m_selectedPdIndex], *m_queue, m_graphicsQueueFamily);
		m_renderer->GetRenderCache().SetLightingCache(
			m_uploader->CreateLightingCache(*m_device, m_physicalDevices[m_selectedPdIndex]));
		m_renderer->SetUploadManager(m_uploader.get());
	}

	void DrawOneFrame(const neurus::RenderConfig* config = nullptr)
	{
		neurus::Scene scene;
		auto camera = CreateTestCamera(kWidth, kHeight);
		scene.UseCamera(camera);
		scene.ActivateCamera(camera->GetObjectID());

		neurus::RenderFrameSnapshot snapshot{};
		neurus::RenderContext context{};
		PublishFrame(m_renderer->GetRenderCache(), context, snapshot, &scene, config);
		ASSERT_NO_THROW(m_renderer->DrawFrame(context));
		m_renderer->WaitIdle();
	}

	std::unique_ptr<neurus::UploadManager> m_uploader;
	std::unique_ptr<neurus::DeferredRenderer> m_renderer;
};

TEST_F(RenderGraphPipelineTest, DefaultSpecCompilesAndDraws)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());

	EXPECT_TRUE(m_renderer->LastGraphReport().compiled);
	EXPECT_EQ(m_renderer->ScenePassCount(), 6u);
	EXPECT_TRUE(m_renderer->LastGraphReport().diagnostics.empty());

	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());
	EXPECT_EQ(m_renderer->ScenePassCount(), 6u);
}

TEST_F(RenderGraphPipelineTest, FxaaSpecHasSevenPasses)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());
	ASSERT_TRUE(m_renderer->CompileMainGraph(neurus::MakeDefaultRenderGraphSpec(), true));
	EXPECT_EQ(m_renderer->ScenePassCount(), 7u);

	neurus::RenderConfig config{};
	config.r_aa = neurus::AAAlg::FXAA;
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame(&config));
	EXPECT_EQ(m_renderer->ScenePassCount(), 7u);
}

TEST_F(RenderGraphPipelineTest, EditedSpecChangesTopology)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());

	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	std::erase_if(spec.nodes, [](const neurus::GraphNodeSpec& node)
	{
		return node.id == "ssao";
	});
	std::erase_if(spec.edges, [](const neurus::GraphEdgeSpec& edge)
	{
		return edge.from == "ssao" || edge.to == "ssao";
	});
	// U_AO still has to be fed by something; the topology test only cares that
	// removing a node changes the executed pass list.
	spec.edges.push_back({"geometry", "gAlbedo", "lighting", "U_AO"});

	ASSERT_TRUE(m_renderer->CompileMainGraph(spec, false));
	EXPECT_EQ(m_renderer->ScenePassCount(), 5u);

	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());
	EXPECT_EQ(m_renderer->ScenePassCount(), 5u);
}

TEST_F(RenderGraphPipelineTest, BadTagKeepsPreviousGraph)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());
	const size_t previousPasses = m_renderer->ScenePassCount();

	neurus::RenderGraphSpec bad = neurus::MakeDefaultRenderGraphSpec();
	bad.FindNode("geometry")->outputs.erase("gPosition");

	EXPECT_FALSE(m_renderer->CompileMainGraph(bad, false));
	EXPECT_FALSE(m_renderer->LastGraphReport().compiled);
	EXPECT_FALSE(m_renderer->LastGraphReport().diagnostics.empty());
	EXPECT_EQ(m_renderer->ScenePassCount(), previousPasses);

	// The previous graph is still the live one; drawing it is valid.
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());
}
