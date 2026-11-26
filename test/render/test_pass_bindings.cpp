/**
 * @file test_pass_bindings.cpp
 * @brief Binding-plan construction from runtime graph sockets.
 */

#include <gtest/gtest.h>

#include "asset/data/render_graph/CPUPorts.hpp"
#include "render/passes/Pass.hpp"
#include "render/render_graph/RenderGraph.hpp"

#include <vector>

using namespace neurus;

namespace {

class SelfManagedPass : public Pass
{
public:
	std::vector<uint32_t> SelfManagedBindings() const override { return {1}; }
};

class PlainPass : public Pass
{
};

RenderGraph::NodeT* AddProducer(RenderGraph& graph, PlainPass& pass)
{
	CPUPorts ports;
	ports.ports = {{"source", PortDirection::Output, "image2D", 0, 0}};
	return graph.AddPass(&pass, ports);
}

} // anonymous namespace

TEST(PassBindings, BuildsImageInputsAndOutputs)
{
	CPUPorts ports;
	ports.ports = {
		{"selfManaged", PortDirection::Input, "sampler2D", 1, 0},
		{"gIn",   PortDirection::Input,  "sampler2D", 0, 0},
		{"gOut",  PortDirection::Output, "image2D",   2, 0},
		{"gFrag", PortDirection::Output, "vec4",      0, 0},
		{"gBuf",  PortDirection::Input,  "uniform",   3, 0},
	};

	PlainPass producer;
	SelfManagedPass pass;
	RenderGraph graph;
	auto* source = AddProducer(graph, producer);
	auto* node = graph.AddPass(&pass, ports, {"gBuf"});
	ASSERT_TRUE(graph.Connect(source, "source", node, "selfManaged"));
	ASSERT_TRUE(graph.Connect(source, "source", node, "gIn"));
	graph.Compile();

	const auto& bindings = node->data.imageBindings;

	ASSERT_EQ(bindings.size(), 2u);

	EXPECT_EQ(bindings[0].port, "gIn");
	EXPECT_EQ(bindings[0].direction, PortDirection::Input);
	EXPECT_EQ(bindings[0].binding, 0u);
	EXPECT_EQ(bindings[0].type, vk::DescriptorType::eCombinedImageSampler);
	EXPECT_EQ(bindings[0].layout, vk::ImageLayout::eShaderReadOnlyOptimal);

	EXPECT_EQ(bindings[1].port, "gOut");
	EXPECT_EQ(bindings[1].direction, PortDirection::Output);
	EXPECT_EQ(bindings[1].binding, 2u);
	EXPECT_EQ(bindings[1].type, vk::DescriptorType::eStorageImage);
	EXPECT_EQ(bindings[1].layout, vk::ImageLayout::eGeneral);
}

TEST(PassBindings, UnconnectedExternalInputHasNoGraphBinding)
{
	CPUPorts ports;
	ports.ports = {
		{"gIn",  PortDirection::Input,  "sampler2D", 0, 0},
		{"gOut", PortDirection::Output, "image2D",   2, 0},
	};

	PlainPass pass;
	RenderGraph graph;
	auto* node = graph.AddPass(&pass, ports, {"gIn"});
	graph.Compile();

	const auto& bindings = node->data.imageBindings;

	ASSERT_EQ(bindings.size(), 1u);
	EXPECT_EQ(bindings[0].port, "gOut");
	EXPECT_EQ(bindings[0].direction, PortDirection::Output);
}

TEST(PassBindings, DefaultPassHasNoSelfManagedBindings)
{
	CPUPorts ports;
	ports.ports = {
		{"gIn", PortDirection::Input, "sampler2D", 1, 0},
	};

	PlainPass producer;
	PlainPass pass;
	RenderGraph graph;
	auto* source = AddProducer(graph, producer);
	auto* node = graph.AddPass(&pass, ports);
	ASSERT_TRUE(graph.Connect(source, "source", node, "gIn"));
	graph.Compile();

	const auto& bindings = node->data.imageBindings;

	ASSERT_EQ(bindings.size(), 1u);
	EXPECT_EQ(bindings[0].port, "gIn");
}
