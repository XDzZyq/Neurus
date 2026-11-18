/**
 * @file test_render_graph_pipeline.cpp
 * @brief The production renderer compiles its scene graph from the CPU document.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.hpp"

#include "core/RenderConfig.hpp"
#include "asset/data/render_graph/RenderGraphData.hpp"
#include "asset/data/render_graph/GraphDocument.hpp"
#include "render/DeferredRenderer.hpp"
#include "render/SharedQueue.hpp"
#include "render/UploadManager.hpp"
#include "render/render_graph/PassFactory.hpp"
#include "render/requests/RenderRequests.hpp"
#include "render/resources/LightingCache.hpp"
#include "scene/Camera.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace
{

constexpr uint32_t kWidth  = 96;
constexpr uint32_t kHeight = 64;

neurus::RenderGraphData::NodeType* FindNode(neurus::RenderGraphData& graph,
                                           const std::string& name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

neurus::RenderGraphData::SocketOutT* FindOutput(neurus::RenderGraphData::NodeType& node,
                                               const std::string& name)
{
	for (const auto& output : node.outputs)
	{
		if (output->name == name)
			return output.get();
	}
	return nullptr;
}

neurus::RenderGraphData::SocketInT* FindInput(neurus::RenderGraphData::NodeType& node,
                                             const std::string& name)
{
	for (const auto& input : node.inputs)
	{
		if (input->name == name)
			return input.get();
	}
	return nullptr;
}

} // anonymous namespace

class RenderGraphPipelineTest : public VulkanTestShared
{
protected:
	void TearDown() override
	{
		m_renderer.reset();
		m_documentFactory.reset();
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

		// A second factory supplies the document's ports. The renderer's own
		// instances are separate; only the port tables are shared knowledge.
		m_documentFactory = std::make_unique<neurus::PassFactory>(
			*m_device, m_physicalDevices[m_selectedPdIndex], 2,
			*m_queue, m_graphicsQueueFamily);
	}

	neurus::RenderGraphData DefaultDocument() const
	{
		return neurus::MakeDefaultCpuGraph(*m_documentFactory);
	}

	void PostDocument(neurus::RenderGraphData graph)
	{
		neurus::CompileRenderGraphRequest request;
		request.graph = std::make_shared<const neurus::RenderGraphData>(std::move(graph));
		m_renderer->Post(neurus::RenderRequest{std::move(request)});
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
	std::unique_ptr<neurus::PassFactory> m_documentFactory;
};

TEST_F(RenderGraphPipelineTest, DefaultDocumentCompilesAndDraws)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());

	// Before the Editor publishes, the renderer has no topology but still draws
	// its overlays instead of throwing.
	EXPECT_EQ(m_renderer->ScenePassCount(), 0u);
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

	ASSERT_NO_FATAL_FAILURE(PostDocument(DefaultDocument()));
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

	EXPECT_TRUE(m_renderer->LastGraphReport().compiled);
	EXPECT_EQ(m_renderer->ScenePassCount(), 6u);  // FXAA off
	EXPECT_TRUE(m_renderer->LastGraphReport().diagnostics.empty());
}

TEST_F(RenderGraphPipelineTest, FxaaFlagAddsThePass)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());
	ASSERT_NO_FATAL_FAILURE(PostDocument(DefaultDocument()));
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());
	ASSERT_EQ(m_renderer->ScenePassCount(), 6u);

	neurus::RenderConfig config{};
	config.r_aa = neurus::AAAlg::FXAA;
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame(&config));
	EXPECT_EQ(m_renderer->ScenePassCount(), 7u);
}

TEST_F(RenderGraphPipelineTest, EditedDocumentChangesTopology)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());

	neurus::RenderGraphData document = DefaultDocument();
	neurus::RenderGraphData::NodeType* ssao = FindNode(document, "ssao");
	neurus::RenderGraphData::NodeType* geometry = FindNode(document, "geometry");
	neurus::RenderGraphData::NodeType* lighting = FindNode(document, "lighting");
	ASSERT_NE(ssao, nullptr);
	ASSERT_NE(geometry, nullptr);
	ASSERT_NE(lighting, nullptr);

	// U_AO still has to be fed by something; the topology test only cares that
	// removing a node changes the executed pass list.
	ASSERT_TRUE(document.RemoveNode(ssao));
	ASSERT_TRUE(document.Connect(FindOutput(*geometry, "gAlbedo"),
	                             FindInput(*lighting, "U_AO")));

	ASSERT_NO_FATAL_FAILURE(PostDocument(std::move(document)));
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

	EXPECT_TRUE(m_renderer->LastGraphReport().compiled);
	EXPECT_EQ(m_renderer->ScenePassCount(), 5u);
}

TEST_F(RenderGraphPipelineTest, BadDocumentKeepsPreviousGraph)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());
	ASSERT_NO_FATAL_FAILURE(PostDocument(DefaultDocument()));
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());
	const size_t previousPasses = m_renderer->ScenePassCount();
	ASSERT_EQ(previousPasses, 6u);

	neurus::RenderGraphData bad = DefaultDocument();
	bad.nodes[0]->outputs[0]->data.tag.reset();

	ASSERT_NO_FATAL_FAILURE(PostDocument(std::move(bad)));
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

	EXPECT_FALSE(m_renderer->LastGraphReport().compiled);
	EXPECT_FALSE(m_renderer->LastGraphReport().diagnostics.empty());
	EXPECT_EQ(m_renderer->ScenePassCount(), previousPasses);
}
