/**
 * @file test_render_graph_document.cpp
 * @brief The CPU graph as a saved document, and its validation.
 */

#include <gtest/gtest.h>

#include "shared/TestPassPorts.hpp"

#include "asset/data/render_graph/RenderGraphRecord.hpp"
#include "asset/data/render_graph/RenderGraphTopology.hpp"
#include "asset/data/ShaderData.hpp"
#include "core/ResourceManager.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"

#include <cereal/archives/json.hpp>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

class TestShader final : public neurus::ShaderData
{
public:
	neurus::ShaderType GetType() const override { return neurus::ShaderType::COMPUTE; }
};

using neurus::project::RenderGraphEdgeRecord;
using neurus::project::RenderGraphNodeRecord;
using neurus::project::RenderGraphRecord;
using neurus::CpuGraphDiagnostic;
using neurus::RenderGraphTopology;

const RenderGraphTopology::NodeType* FindNode(const RenderGraphTopology& graph,
                                         const std::string& name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

RenderGraphTopology::NodeType* FindNode(RenderGraphTopology& graph, const std::string& name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

std::string Serialize(const RenderGraphRecord& record)
{
	std::ostringstream out;
	{
		cereal::JSONOutputArchive ar(out);
		ar(cereal::make_nvp("graph", record));
	}
	return out.str();
}

RenderGraphRecord Deserialize(const std::string& text)
{
	std::istringstream in(text);
	cereal::JSONInputArchive ar(in);
	RenderGraphRecord record;
	ar(cereal::make_nvp("graph", record));
	return record;
}

bool HasDiagnostic(const std::vector<CpuGraphDiagnostic>& diagnostics,
                   const std::string& node, const std::string& fragment)
{
	return std::any_of(diagnostics.begin(), diagnostics.end(),
		[&](const CpuGraphDiagnostic& diagnostic)
		{
			return diagnostic.node == node
			    && diagnostic.message.find(fragment) != std::string::npos;
		});
}

bool HasProblem(const std::vector<std::string>& problems, const std::string& fragment)
{
	return std::any_of(problems.begin(), problems.end(),
		[&](const std::string& problem)
		{
			return problem.find(fragment) != std::string::npos;
		});
}

} // anonymous namespace

TEST(RenderGraphDocument, DefaultGraph)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	const RenderGraphTopology graph = neurus::RenderGraphTopology::Default(ports);

	ASSERT_EQ(graph.nodes.size(), 7u);
	const std::vector<std::string> expected = {
		"geometry", "shadow_depth", "shadow_intensity", "ssao",
		"lighting", "compose", "fxaa",
	};
	for (size_t i = 0; i < expected.size(); ++i)
		EXPECT_EQ(graph.nodes[i]->name, expected[i]);

	for (const auto& node : graph.nodes)
	{
		EXPECT_EQ(node->data.shader, nullptr);
		for (const auto& output : node->outputs)
		{
			EXPECT_TRUE(output->data.tag.has_value())
				<< node->name << "." << output->name;
		}
	}

	// FXAA is the only node whose outputs are all unconsumed (an unused output
	// such as the G-Buffer depth is still allowed).
	for (const auto& node : graph.nodes)
	{
		const bool consumed = std::any_of(
			node->outputs.begin(), node->outputs.end(),
			[](const auto& output) { return !output->targets.empty(); });
		if (!consumed)
			EXPECT_EQ(node->name, "fxaa") << node->name;
	}

	EXPECT_TRUE(neurus::ValidateCpuGraph(graph, ports).empty());
}

TEST(RenderGraphDocument, RoundTripPreservesTopology)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	const RenderGraphTopology graph = neurus::RenderGraphTopology::Default(ports);
	const RenderGraphRecord original = neurus::project::ToRecord(graph);

	const RenderGraphRecord restored = Deserialize(Serialize(original));
	EXPECT_EQ(restored, original);

	std::vector<std::string> problems;
	const RenderGraphTopology copy = neurus::project::FromRecord(restored, ports, problems);
	EXPECT_TRUE(problems.empty());
	EXPECT_EQ(neurus::project::ToRecord(copy), original);

	EXPECT_EQ(copy.nodes.size(), graph.nodes.size());
	for (size_t i = 0; i < copy.nodes.size(); ++i)
	{
		EXPECT_EQ(copy.nodes[i]->name, graph.nodes[i]->name);
		ASSERT_EQ(copy.nodes[i]->inputs.size(), graph.nodes[i]->inputs.size());
		ASSERT_EQ(copy.nodes[i]->outputs.size(), graph.nodes[i]->outputs.size());
		for (size_t p = 0; p < copy.nodes[i]->outputs.size(); ++p)
		{
			EXPECT_EQ(copy.nodes[i]->outputs[p]->name,
			          graph.nodes[i]->outputs[p]->name);
		}
	}
}

TEST(RenderGraphDocument, InputsComeFromShaders)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();

	RenderGraphRecord record;
	neurus::project::RenderGraphNodeRecord node;
	node.name = "lighting";
	node.pass = "LightingPass";
	record.nodes.push_back(node);

	std::vector<std::string> problems;
	const RenderGraphTopology graph = neurus::project::FromRecord(record, ports, problems);
	EXPECT_TRUE(problems.empty());

	ASSERT_EQ(graph.nodes.size(), 1u);
	ASSERT_EQ(graph.nodes[0]->inputs.size(), 8u);
	EXPECT_EQ(graph.nodes[0]->inputs[0]->name, "gPosition");
	EXPECT_TRUE(graph.nodes[0]->outputs.empty());
}

TEST(RenderGraphDocument, DoubleProducerIsReportedAtLoad)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();

	RenderGraphRecord record;

	neurus::project::RenderGraphNodeRecord geometry;
	geometry.name = "geometry";
	geometry.pass = "GeometryPass";
	geometry.outputs = {
		{"gPosition", "vec4", 0, 0, neurus::GraphOutputTag{
			neurus::GraphFormat::Rgba16f, neurus::GraphUsage::ColorSampled}},
		{"gNormal", "vec4", 0, 1, neurus::GraphOutputTag{
			neurus::GraphFormat::Rgba16f, neurus::GraphUsage::ColorSampled}},
	};
	record.nodes.push_back(geometry);

	neurus::project::RenderGraphNodeRecord ssao;
	ssao.name = "ssao";
	ssao.pass = "SSAOPass";
	record.nodes.push_back(ssao);

	record.edges = {
		{"geometry", "gPosition", "ssao", "gPosition"},
		{"geometry", "gNormal", "ssao", "gPosition"},
	};

	std::vector<std::string> problems;
	const RenderGraphTopology graph = neurus::project::FromRecord(record, ports, problems);
	EXPECT_EQ(graph.nodes.size(), 2u);
	ASSERT_EQ(problems.size(), 1u);
	EXPECT_NE(problems[0].find("more than one producer"), std::string::npos);
}

TEST(RenderGraphDocument, UnknownPassIsNamed)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();

	RenderGraphRecord record;
	neurus::project::RenderGraphNodeRecord node;
	node.name = "bogus";
	node.pass = "NopePass";
	record.nodes.push_back(node);

	std::vector<std::string> problems;
	const RenderGraphTopology graph = neurus::project::FromRecord(record, ports, problems);
	EXPECT_TRUE(HasProblem(problems, "unknown pass kind"));

	const std::vector<CpuGraphDiagnostic> diagnostics =
		neurus::ValidateCpuGraph(graph, ports);
	EXPECT_TRUE(HasDiagnostic(diagnostics, "bogus", "unknown pass kind"));
}

TEST(CpuGraphValidation, MissingTagIsNamed)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	RenderGraphTopology graph = neurus::RenderGraphTopology::Default(ports);

	RenderGraphTopology::NodeType* geometry = FindNode(graph, "geometry");
	ASSERT_NE(geometry, nullptr);
	geometry->outputs[0]->data.tag.reset();

	const std::vector<CpuGraphDiagnostic> diagnostics =
		neurus::ValidateCpuGraph(graph, ports);
	EXPECT_TRUE(HasDiagnostic(diagnostics, "geometry", "no tag"));
}

TEST(CpuGraphValidation, StaleOutputPortIsReported)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	RenderGraphTopology graph = neurus::RenderGraphTopology::Default(ports);

	RenderGraphTopology::NodeType* geometry = FindNode(graph, "geometry");
	ASSERT_NE(geometry, nullptr);
	geometry->AddOutput("ghost")->data =
		neurus::CpuPort{"ghost", neurus::PortDirection::Output, "vec4", 0, 0,
		                neurus::GraphOutputTag{neurus::GraphFormat::Rgba16f,
		                                       neurus::GraphUsage::ColorSampled}};

	const std::vector<CpuGraphDiagnostic> diagnostics =
		neurus::ValidateCpuGraph(graph, ports);
	EXPECT_TRUE(HasDiagnostic(diagnostics, "geometry", "no longer declared"));
}

TEST(CpuGraphValidation, CycleIsNamed)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	RenderGraphTopology graph = neurus::RenderGraphTopology::Default(ports);

	// Wire a back edge by hand: Connect refuses a cycle, the public sockets do
	// not, and validation must still name it.
	RenderGraphTopology::NodeType* geometry = FindNode(graph, "geometry");
	RenderGraphTopology::NodeType* lighting = FindNode(graph, "lighting");
	ASSERT_NE(geometry, nullptr);
	ASSERT_NE(lighting, nullptr);

	RenderGraphTopology::SocketOutT* lightingOut = lighting->outputs[0].get();
	RenderGraphTopology::SocketInT* geometryIn = geometry->inputs.empty()
		? geometry->AddInput("gInput")
		: geometry->inputs[0].get();

	lightingOut->targets.push_back(geometryIn);
	geometryIn->source = lightingOut;

	const std::vector<CpuGraphDiagnostic> diagnostics =
		neurus::ValidateCpuGraph(graph, ports);
	const bool named = std::any_of(diagnostics.begin(), diagnostics.end(),
		[](const CpuGraphDiagnostic& diagnostic)
		{
			return diagnostic.message.find("cycle") != std::string::npos;
		});
	EXPECT_TRUE(named);
}

TEST(RenderGraphDocument, SavedShaderUidResolvesToOwnedPoolResource)
{
	const auto ports = neurus::test::MakeRealPortSource();
	neurus::ResourceManager resources;
	auto shader = resources.Load<TestShader>();
	auto& stage = shader->GetStage(neurus::ShaderType::COMPUTE);
	stage.code = "restored content";
	stage.spv = {1, 2, 3};
	RenderGraphTopology graph = RenderGraphTopology::Default(ports);
	graph.nodes[3]->data.shader = shader;
	const auto record = Deserialize(Serialize(neurus::project::ToRecord(graph)));
	EXPECT_EQ(record.nodes[3].shaderUid, shader->GetObjectID());
	EXPECT_EQ(record.nodes[0].shaderUid, 0);

	std::vector<std::string> problems;
	const auto restored = neurus::project::FromRecord(record, ports, problems, &resources);
	EXPECT_TRUE(problems.empty());
	ASSERT_EQ(restored.nodes.size(), graph.nodes.size());
	EXPECT_EQ(restored.nodes[3]->data.shader, shader);
	EXPECT_EQ(restored.nodes[0]->data.shader, nullptr);
	EXPECT_EQ(restored.Hash(), graph.Hash());
	EXPECT_EQ(neurus::project::ToRecord(restored), record);
}

TEST(RenderGraphDocument, MissingWrongTypeAndInvalidShadersSelectDefaults)
{
	const auto ports = neurus::test::MakeRealPortSource();
	neurus::ResourceManager resources;
	const auto invalid = resources.Load<TestShader>();
	const auto wrongType = resources.Load<neurus::UID>();
	auto record = neurus::project::ToRecord(RenderGraphTopology::Default(ports));
	record.nodes[0].shaderUid = invalid->GetObjectID();
	record.nodes[1].shaderUid = wrongType->GetObjectID();
	record.nodes[2].shaderUid = 2147483647;

	std::vector<std::string> problems;
	const auto restored = neurus::project::FromRecord(record, ports, problems, &resources);
	EXPECT_TRUE(problems.empty());
	ASSERT_EQ(restored.nodes.size(), 7u);
	for (const auto& node : restored.nodes)
		EXPECT_EQ(node->data.shader, nullptr);
	for (const auto& node : neurus::project::ToRecord(restored).nodes)
		EXPECT_EQ(node.shaderUid, 0);
	EXPECT_EQ(resources.Get<TestShader>(invalid->GetObjectID()), invalid);

	const auto withoutPool = neurus::project::FromRecord(record, ports, problems);
	for (const auto& node : withoutPool.nodes)
		EXPECT_EQ(node->data.shader, nullptr);
}
