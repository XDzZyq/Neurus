/**
 * @file test_graph_document.cpp
 * @brief The CPU graph as a saved document, and its validation.
 */

#include <gtest/gtest.h>

#include "shared/TestPassPorts.hpp"

#include "asset/graph/CpuGraphValidation.hpp"
#include "asset/graph/GraphDocument.hpp"

#include <cereal/archives/json.hpp>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using neurus::CpuGraphRecord;
using neurus::CpuGraphDiagnostic;
using neurus::CpuRenderGraph;

const CpuRenderGraph::NodeType* FindNode(const CpuRenderGraph& graph,
                                         const std::string& name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

CpuRenderGraph::NodeType* FindNode(CpuRenderGraph& graph, const std::string& name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

std::string Serialize(const CpuGraphRecord& record)
{
	std::ostringstream out;
	{
		cereal::JSONOutputArchive ar(out);
		ar(cereal::make_nvp("graph", record));
	}
	return out.str();
}

CpuGraphRecord Deserialize(const std::string& text)
{
	std::istringstream in(text);
	cereal::JSONInputArchive ar(in);
	CpuGraphRecord record;
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

TEST(GraphDocument, DefaultGraph)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	const CpuRenderGraph graph = neurus::MakeDefaultCpuGraph(ports);

	ASSERT_EQ(graph.nodes.size(), 7u);
	const std::vector<std::string> expected = {
		"geometry", "shadow_depth", "shadow_intensity", "ssao",
		"lighting", "compose", "fxaa",
	};
	for (size_t i = 0; i < expected.size(); ++i)
		EXPECT_EQ(graph.nodes[i]->name, expected[i]);

	for (const auto& node : graph.nodes)
	{
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

TEST(GraphDocument, RoundTripPreservesTopology)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	const CpuRenderGraph graph = neurus::MakeDefaultCpuGraph(ports);
	const CpuGraphRecord original = neurus::ToRecord(graph);

	const CpuGraphRecord restored = Deserialize(Serialize(original));
	EXPECT_EQ(restored, original);

	std::vector<std::string> problems;
	const CpuRenderGraph copy = neurus::FromRecord(restored, ports, problems);
	EXPECT_TRUE(problems.empty());
	EXPECT_EQ(neurus::ToRecord(copy), original);

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

TEST(GraphDocument, InputsComeFromShaders)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();

	CpuGraphRecord record;
	neurus::CpuNodeRecord node;
	node.name = "lighting";
	node.pass = "LightingPass";
	record.nodes.push_back(node);

	std::vector<std::string> problems;
	const CpuRenderGraph graph = neurus::FromRecord(record, ports, problems);
	EXPECT_TRUE(problems.empty());

	ASSERT_EQ(graph.nodes.size(), 1u);
	ASSERT_EQ(graph.nodes[0]->inputs.size(), 8u);
	EXPECT_EQ(graph.nodes[0]->inputs[0]->name, "gPosition");
	EXPECT_TRUE(graph.nodes[0]->outputs.empty());
}

TEST(GraphDocument, ConditionPruning)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	CpuRenderGraph graph = neurus::MakeDefaultCpuGraph(ports);

	neurus::ApplyConditions(graph, false);
	ASSERT_EQ(graph.nodes.size(), 6u);
	EXPECT_EQ(FindNode(graph, "fxaa"), nullptr);
	EXPECT_TRUE(neurus::ValidateCpuGraph(graph, ports).empty());

	neurus::ApplyConditions(graph, true);
	EXPECT_EQ(graph.nodes.size(), 6u);
}

TEST(GraphDocument, DoubleProducerIsReportedAtLoad)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();

	CpuGraphRecord record;

	neurus::CpuNodeRecord geometry;
	geometry.name = "geometry";
	geometry.pass = "GeometryPass";
	geometry.outputs = {
		{"gPosition", "vec4", 0, 0, neurus::GraphOutputTag{
			neurus::GraphFormat::Rgba16f, neurus::GraphUsage::ColorSampled}},
		{"gNormal", "vec4", 0, 1, neurus::GraphOutputTag{
			neurus::GraphFormat::Rgba16f, neurus::GraphUsage::ColorSampled}},
	};
	record.nodes.push_back(geometry);

	neurus::CpuNodeRecord ssao;
	ssao.name = "ssao";
	ssao.pass = "SSAOPass";
	record.nodes.push_back(ssao);

	record.edges = {
		{"geometry", "gPosition", "ssao", "gPosition"},
		{"geometry", "gNormal", "ssao", "gPosition"},
	};

	std::vector<std::string> problems;
	const CpuRenderGraph graph = neurus::FromRecord(record, ports, problems);
	EXPECT_EQ(graph.nodes.size(), 2u);
	ASSERT_EQ(problems.size(), 1u);
	EXPECT_NE(problems[0].find("more than one producer"), std::string::npos);
}

TEST(GraphDocument, UnknownPassIsNamed)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();

	CpuGraphRecord record;
	neurus::CpuNodeRecord node;
	node.name = "bogus";
	node.pass = "NopePass";
	record.nodes.push_back(node);

	std::vector<std::string> problems;
	const CpuRenderGraph graph = neurus::FromRecord(record, ports, problems);
	EXPECT_TRUE(HasProblem(problems, "unknown pass kind"));

	const std::vector<CpuGraphDiagnostic> diagnostics =
		neurus::ValidateCpuGraph(graph, ports);
	EXPECT_TRUE(HasDiagnostic(diagnostics, "bogus", "unknown pass kind"));
}

TEST(CpuGraphValidation, MissingTagIsNamed)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	CpuRenderGraph graph = neurus::MakeDefaultCpuGraph(ports);

	CpuRenderGraph::NodeType* geometry = FindNode(graph, "geometry");
	ASSERT_NE(geometry, nullptr);
	geometry->outputs[0]->data.tag.reset();

	const std::vector<CpuGraphDiagnostic> diagnostics =
		neurus::ValidateCpuGraph(graph, ports);
	EXPECT_TRUE(HasDiagnostic(diagnostics, "geometry", "no tag"));
}

TEST(CpuGraphValidation, StaleOutputPortIsReported)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	CpuRenderGraph graph = neurus::MakeDefaultCpuGraph(ports);

	CpuRenderGraph::NodeType* geometry = FindNode(graph, "geometry");
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
	CpuRenderGraph graph = neurus::MakeDefaultCpuGraph(ports);

	// Wire a back edge by hand: Connect refuses a cycle, the public sockets do
	// not, and validation must still name it.
	CpuRenderGraph::NodeType* geometry = FindNode(graph, "geometry");
	CpuRenderGraph::NodeType* lighting = FindNode(graph, "lighting");
	ASSERT_NE(geometry, nullptr);
	ASSERT_NE(lighting, nullptr);

	CpuRenderGraph::SocketOutT* lightingOut = lighting->outputs[0].get();
	CpuRenderGraph::SocketInT* geometryIn = geometry->inputs.empty()
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
