/**
 * @file test_graph_uploader.cpp
 * @brief The CPU document uploads onto the runtime graph through the morphism.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.hpp"

#include "asset/data/render_graph/CpuRenderGraph.hpp"
#include "asset/graph/GraphDocument.hpp"
#include "render/render_graph/GraphUploader.hpp"
#include "render/render_graph/PassFactory.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

neurus::CpuRenderGraph::NodeType* FindNode(neurus::CpuRenderGraph& graph,
                                           const std::string& name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

bool HasError(const std::vector<neurus::CpuGraphDiagnostic>& errors,
              const std::string& node, const std::string& fragment)
{
	return std::any_of(errors.begin(), errors.end(),
		[&](const neurus::CpuGraphDiagnostic& error)
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
	}

	void TearDown() override
	{
		m_factory.reset();
		VulkanTestShared::TearDown();
	}

	neurus::CpuRenderGraph DefaultDocument() const
	{
		return neurus::MakeDefaultCpuGraph(*m_factory);
	}

	std::unique_ptr<neurus::PassFactory> m_factory;
};

TEST_F(GraphUploaderTest, DefaultDocumentIsTotal)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const neurus::CpuRenderGraph document = DefaultDocument();
	const neurus::UploadResult upload = neurus::UploadCpuGraph(
		document, *m_factory, std::span<const neurus::ShaderSnapshot>{});

	std::string problems;
	for (const neurus::CpuGraphDiagnostic& error : upload.errors)
		problems += error.node + "." + error.port + ": " + error.message + "\n";

	ASSERT_TRUE(upload.errors.empty()) << problems;
	ASSERT_NE(upload.graph, nullptr);
	EXPECT_TRUE(upload.morphism.IsTotal());
	EXPECT_TRUE(upload.morphism.PreservesIncidence());
	EXPECT_TRUE(upload.morphism.Validate().Ok());
	EXPECT_EQ(upload.graph->PassCount(), 7u);
}

TEST_F(GraphUploaderTest, NodeAndOrderMatch)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const neurus::CpuRenderGraph document = DefaultDocument();
	const neurus::UploadResult upload = neurus::UploadCpuGraph(
		document, *m_factory, std::span<const neurus::ShaderSnapshot>{});
	ASSERT_NE(upload.graph, nullptr);
	ASSERT_TRUE(upload.errors.empty());

	const std::vector<neurus::CpuRenderGraph::NodeType*> cpuOrder =
		document.TopologicalSort();
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

	neurus::CpuRenderGraph document = DefaultDocument();
	FindNode(document, "ssao")->data.pass = "NopePass";

	const neurus::UploadResult upload = neurus::UploadCpuGraph(
		document, *m_factory, std::span<const neurus::ShaderSnapshot>{});

	EXPECT_EQ(upload.graph, nullptr);
	EXPECT_TRUE(HasError(upload.errors, "ssao", "unknown pass kind"));
}

TEST_F(GraphUploaderTest, MissingShaderSnapshotIsNamed)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	neurus::CpuRenderGraph document = DefaultDocument();
	FindNode(document, "lighting")->data.shaderUid = 4242;

	const neurus::UploadResult upload = neurus::UploadCpuGraph(
		document, *m_factory, std::span<const neurus::ShaderSnapshot>{});

	EXPECT_EQ(upload.graph, nullptr);
	EXPECT_TRUE(HasError(upload.errors, "lighting", "no shader snapshot"));
}

TEST_F(GraphUploaderTest, StaleSocketIsNamed)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	neurus::CpuRenderGraph document = DefaultDocument();
	neurus::CpuRenderGraph::NodeType* geometry = FindNode(document, "geometry");
	ASSERT_NE(geometry, nullptr);
	geometry->AddOutput("ghost")->data =
		neurus::CpuPort{"ghost", neurus::PortDirection::Output, "vec4", 0, 0,
		                neurus::GraphOutputTag{neurus::GraphFormat::Rgba16f,
		                                       neurus::GraphUsage::ColorSampled}};

	const neurus::UploadResult upload = neurus::UploadCpuGraph(
		document, *m_factory, std::span<const neurus::ShaderSnapshot>{});

	EXPECT_EQ(upload.graph, nullptr);
	EXPECT_TRUE(HasError(upload.errors, "geometry", "no longer declared"));
}

TEST_F(GraphUploaderTest, EmptyDocumentUploads)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const neurus::CpuRenderGraph empty;
	const neurus::UploadResult upload = neurus::UploadCpuGraph(
		empty, *m_factory, std::span<const neurus::ShaderSnapshot>{});

	EXPECT_TRUE(upload.errors.empty());
	ASSERT_NE(upload.graph, nullptr);
	EXPECT_EQ(upload.graph->PassCount(), 0u);
	EXPECT_TRUE(upload.morphism.IsTotal());
}
