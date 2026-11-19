/**
 * @file test_graph_uploader.cpp
 * @brief The upload service maps a document onto the runtime graph.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.hpp"

#include "render/RenderCache.hpp"
#include "render/render_graph/GraphUploader.hpp"
#include "render/render_graph/PassFactory.hpp"
#include "render/render_graph/RenderGraph.hpp"
#include "render_api/RenderRequests.hpp"

#include "asset/data/RenderGraphData.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

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

bool HasError(const std::vector<neurus::GraphDiagnostic>& errors,
              const std::string& node, const std::string& fragment)
{
	return std::any_of(errors.begin(), errors.end(),
		[&](const neurus::GraphDiagnostic& error)
		{
			return error.node == node
			    && error.message.find(fragment) != std::string::npos;
		});
}

} // anonymous namespace

class GraphUploaderTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!HasVulkan())
			return;

		m_factory = std::make_unique<neurus::PassFactory>(
			*m_device, m_physicalDevices[m_selectedPdIndex], 2,
			*m_queue, m_graphicsQueueFamily);
		m_cache = std::make_unique<neurus::RenderCache>(
			*m_device, m_physicalDevices[m_selectedPdIndex]);
		m_uploader = std::make_unique<neurus::GraphUploader>(*m_factory);
	}

	void TearDown() override
	{
		m_uploader.reset();
		m_cache.reset();
		m_factory.reset();
		VulkanTestShared::TearDown();
	}

	neurus::RenderGraphData DefaultDocument() const
	{
		return neurus::RenderGraphData::Default(*m_factory);
	}

	void SetPending(neurus::RenderGraphData document)
	{
		neurus::CompileRenderGraphRequest request;
		request.graph = std::make_shared<const neurus::RenderGraphData>(std::move(document));
		m_uploader->SetPending(request);
		m_uploader->ConsumeNewDocument();
	}

	neurus::GraphUploadResult Compile(neurus::RenderGraphData document)
	{
		SetPending(std::move(document));
		return m_uploader->Compile(*m_cache);
	}

	std::unique_ptr<neurus::PassFactory> m_factory;
	std::unique_ptr<neurus::RenderCache> m_cache;
	std::unique_ptr<neurus::GraphUploader> m_uploader;
};

TEST_F(GraphUploaderTest, DefaultDocumentCompiles)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	neurus::GraphUploadResult upload = Compile(DefaultDocument());

	std::string problems;
	for (const neurus::GraphDiagnostic& error : upload.errors)
		problems += error.node + "." + error.port + ": " + error.message + "\n";

	ASSERT_TRUE(upload.errors.empty()) << problems;
	ASSERT_NE(upload.graph, nullptr);
	EXPECT_TRUE(upload.fxaaPresent);
	EXPECT_EQ(upload.graph->PassCount(), 7u);
}

TEST_F(GraphUploaderTest, DocumentWithoutFxaaKeepsSixPasses)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	neurus::RenderGraphData document = DefaultDocument();
	document.RemoveNode(FindNode(document, "fxaa"));

	neurus::GraphUploadResult upload = Compile(std::move(document));

	ASSERT_TRUE(upload.errors.empty());
	ASSERT_NE(upload.graph, nullptr);
	EXPECT_FALSE(upload.fxaaPresent);
	EXPECT_EQ(upload.graph->PassCount(), 6u);
}

TEST_F(GraphUploaderTest, NodeAndOrderMatch)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	neurus::RenderGraphData document = DefaultDocument();
	const std::vector<neurus::RenderGraphData::NodeType*> cpuOrder =
		document.TopologicalSort();

	neurus::GraphUploadResult upload = Compile(std::move(document));
	ASSERT_NE(upload.graph, nullptr);
	ASSERT_TRUE(upload.errors.empty());

	ASSERT_EQ(upload.graph->CompiledOrder().size(), cpuOrder.size());
	for (size_t i = 0; i < cpuOrder.size(); ++i)
	{
		EXPECT_EQ(upload.graph->CompiledOrder()[i]->name, cpuOrder[i]->name);
		EXPECT_EQ(upload.graph->CompiledOrder()[i]->data.id, cpuOrder[i]->name);
	}
}

TEST_F(GraphUploaderTest, UnknownPassIsNamed)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	neurus::RenderGraphData document = DefaultDocument();
	FindNode(document, "ssao")->data.pass = "NopePass";

	neurus::GraphUploadResult upload = Compile(std::move(document));

	EXPECT_EQ(upload.graph, nullptr);
	EXPECT_TRUE(HasError(upload.errors, "ssao", "unknown pass kind"));
}

TEST_F(GraphUploaderTest, MissingShaderSnapshotIsNamed)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	neurus::RenderGraphData document = DefaultDocument();
	FindNode(document, "lighting")->data.shaderUid = 4242;

	neurus::GraphUploadResult upload = Compile(std::move(document));

	EXPECT_EQ(upload.graph, nullptr);
	EXPECT_TRUE(HasError(upload.errors, "lighting", "no shader snapshot"));
}

TEST_F(GraphUploaderTest, StaleSocketIsNamed)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	neurus::RenderGraphData document = DefaultDocument();
	neurus::RenderGraphData::NodeType* geometry = FindNode(document, "geometry");
	ASSERT_NE(geometry, nullptr);
	geometry->AddOutput("ghost")->data =
		neurus::CpuPort{"ghost", neurus::PortDirection::Output, "vec4", 0, 0,
		                neurus::GraphOutputTag{neurus::GraphFormat::Rgba16f,
		                                       neurus::GraphUsage::ColorSampled}};

	neurus::GraphUploadResult upload = Compile(std::move(document));

	EXPECT_EQ(upload.graph, nullptr);
	EXPECT_TRUE(HasError(upload.errors, "geometry", "no longer declared"));
}

TEST_F(GraphUploaderTest, EmptyDocumentCompiles)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	neurus::GraphUploadResult upload = Compile(neurus::RenderGraphData{});

	EXPECT_TRUE(upload.errors.empty());
	ASSERT_NE(upload.graph, nullptr);
	EXPECT_EQ(upload.graph->PassCount(), 0u);
	EXPECT_FALSE(upload.fxaaPresent);
}

TEST_F(GraphUploaderTest, RejectsBeforeAnyDocument)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	neurus::GraphUploadResult upload = m_uploader->Compile(*m_cache);

	EXPECT_EQ(upload.graph, nullptr);
	ASSERT_EQ(upload.errors.size(), 1u);
	EXPECT_NE(upload.errors[0].message.find("no render graph document"), std::string::npos);
}
