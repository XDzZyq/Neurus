/**
 * @file test_cpu_render_graph.cpp
 * @brief The CPU graph document: shader ports on sockets, clone through the template.
 */

#include <gtest/gtest.h>

#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/ResourceManager.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace {

class TestShader final : public neurus::ShaderData
{
public:
	neurus::ShaderType GetType() const override { return neurus::ShaderType::COMPUTE; }
};

neurus::CpuPort OutputPort(const std::string& name, const std::string& type,
                           const neurus::GraphFormat format,
                           const neurus::GraphUsage usage)
{
	neurus::CpuPort port;
	port.name      = name;
	port.direction = neurus::PortDirection::Output;
	port.type      = type;
	port.tag       = neurus::GraphOutputTag{format, usage,
	                                        neurus::GraphExtent::Render,
	                                        neurus::GraphLayers::Single};
	return port;
}

neurus::CpuPort InputPort(const std::string& name, const std::uint32_t binding)
{
	neurus::CpuPort port;
	port.name      = name;
	port.direction = neurus::PortDirection::Input;
	port.type      = "sampler2D";
	port.binding   = binding;
	return port;
}

void SetInput(neurus::RenderGraphData::SocketInT* socket, const neurus::CpuPort& port)
{
	socket->data = port;
}

void SetOutput(neurus::RenderGraphData::SocketOutT* socket, const neurus::CpuPort& port)
{
	socket->data = port;
}

} // anonymous namespace

TEST(RenderGraphData, SocketsCarryPassPorts)
{
	neurus::ResourceManager resources;
	neurus::RenderGraphData graph;
	neurus::CpuNode pass;
	pass.pass = "GeometryPass";
	pass.shader = resources.Load<TestShader>();
	pass.x = 40.0f;
	pass.y = 60.0f;

	neurus::RenderGraphData::NodeType* node = graph.AddNode("geometry", pass);
	SetOutput(node->AddOutput("gPosition"),
	          OutputPort("gPosition", "vec4",
	                     neurus::GraphFormat::Rgba16f, neurus::GraphUsage::ColorSampled));
	SetInput(node->AddInput("gPrevious"), InputPort("gPrevious", 3));

	const neurus::RenderGraphData::NodeType& stored = *graph.nodes[0];
	EXPECT_EQ(stored.name, "geometry");
	EXPECT_EQ(stored.data.pass, "GeometryPass");
	EXPECT_EQ(stored.data.shader, pass.shader);
	EXPECT_FLOAT_EQ(stored.data.x, 40.0f);
	EXPECT_FLOAT_EQ(stored.data.y, 60.0f);

	ASSERT_EQ(stored.outputs.size(), 1u);
	const neurus::GraphOutputTag& tag = *stored.outputs[0]->data.tag;
	EXPECT_EQ(tag.format, neurus::GraphFormat::Rgba16f);
	EXPECT_EQ(tag.usage, neurus::GraphUsage::ColorSampled);

	ASSERT_EQ(stored.inputs.size(), 1u);
	EXPECT_EQ(stored.inputs[0]->data.direction, neurus::PortDirection::Input);
	EXPECT_EQ(stored.inputs[0]->data.binding, 3u);
	EXPECT_FALSE(stored.inputs[0]->data.tag.has_value());
}

TEST(RenderGraphData, ClonePreservesPortsAndTags)
{
	neurus::ResourceManager resources;
	neurus::RenderGraphData graph;

	neurus::CpuNode geometry;
	geometry.pass = "GeometryPass";
	neurus::RenderGraphData::NodeType* source = graph.AddNode("geometry", geometry);
	SetOutput(source->AddOutput("gPosition"),
	          OutputPort("gPosition", "vec4",
	                     neurus::GraphFormat::Rgba16f, neurus::GraphUsage::ColorSampled));

	neurus::CpuNode ssao;
	ssao.pass = "SSAOPass";
	ssao.shader = resources.Load<TestShader>();
	neurus::RenderGraphData::NodeType* consumer = graph.AddNode("ssao", ssao);
	SetInput(consumer->AddInput("gPosition"), InputPort("gPosition", 0));
	SetOutput(consumer->AddOutput("outputSSAO"),
	          OutputPort("outputSSAO", "image2D",
	                     neurus::GraphFormat::R8Unorm, neurus::GraphUsage::StorageSampled));

	ASSERT_TRUE(graph.Connect(source->outputs[0].get(), consumer->inputs[0].get()));

	const auto copy = graph.Clone();

	ASSERT_EQ(copy->nodes.size(), 2u);
	EXPECT_EQ(copy->nodes[0]->name, "geometry");
	EXPECT_EQ(copy->nodes[1]->name, "ssao");
	EXPECT_EQ(copy->nodes[1]->data.shader, ssao.shader);

	ASSERT_EQ(copy->nodes[0]->outputs.size(), 1u);
	ASSERT_TRUE(copy->nodes[0]->outputs[0]->data.tag.has_value());
	EXPECT_EQ(copy->nodes[0]->outputs[0]->data.tag->format, neurus::GraphFormat::Rgba16f);

	ASSERT_EQ(copy->nodes[1]->outputs.size(), 1u);
	ASSERT_TRUE(copy->nodes[1]->outputs[0]->data.tag.has_value());
	EXPECT_EQ(copy->nodes[1]->outputs[0]->data.tag->usage, neurus::GraphUsage::StorageSampled);

	ASSERT_EQ(copy->nodes[1]->inputs.size(), 1u);
	ASSERT_TRUE(copy->nodes[1]->inputs[0]->IsConnected());
	EXPECT_EQ(copy->nodes[1]->inputs[0]->incoming.from->owner->name, "geometry");
	EXPECT_EQ(copy->nodes[1]->inputs[0]->data.binding, 0u);
}

TEST(RenderGraphData, GraphAndCloneRetainShaderAfterPoolRemoval)
{
	neurus::ResourceManager resources;
	auto shader = resources.Load<TestShader>();
	const int shaderUid = shader->GetObjectID();
	const std::weak_ptr<const neurus::ShaderData> lifetime = shader;
	neurus::RenderGraphData graph;
	neurus::CpuNode data;
	data.shader = shader;
	graph.AddNode("custom", data);
	auto copy = graph.Clone();
	const auto record = neurus::project::ToRecord(graph);
	data.shader = nullptr;
	shader.reset();
	resources.Clear();
	graph.nodes.clear();
	EXPECT_FALSE(lifetime.expired());
	copy->nodes.clear();
	EXPECT_TRUE(lifetime.expired()) << "A saved node keeps the shader UID without owning the shader";
	EXPECT_EQ(record.nodes.front().shaderUid, shaderUid);
}

TEST(RenderGraphData, HashIncludesShaderIdentityButExcludesShaderContentAndLayout)
{
	neurus::ResourceManager resources;
	neurus::RenderGraphData graph;
	auto* node = graph.AddNode("custom");
	const auto defaultHash = graph.Hash();
	auto shader = resources.Load<TestShader>();
	node->data.shader = shader;
	const auto selectedHash = graph.Hash();
	EXPECT_NE(selectedHash, defaultHash);
	node->data.x = 42.0f;
	node->data.y = 17.0f;
	EXPECT_EQ(graph.Hash(), selectedHash);

	auto& stage = shader->GetStage(neurus::ShaderType::COMPUTE);
	stage.code = "first content";
	stage.spv = {1, 2, 3};
	EXPECT_EQ(graph.Hash(), selectedHash);

	// Graph publication preserves ownership and identity. GPU upload values,
	// rather than graph copies, isolate queued bytes from later CPU edits.
	auto published = graph.Clone();
	EXPECT_EQ(published->nodes[0]->data.shader.get(), shader.get());
	stage.spv.back() = 4;
	EXPECT_EQ(graph.Hash(), selectedHash);
	EXPECT_EQ(published->Hash(), selectedHash);

	node->data.shader = resources.Load<TestShader>();
	EXPECT_NE(graph.Hash(), selectedHash);
	node->data.shader = nullptr;
	EXPECT_EQ(graph.Hash(), defaultHash);
}
