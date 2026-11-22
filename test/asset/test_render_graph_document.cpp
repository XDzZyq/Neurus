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

TEST(RenderGraphDocument, LegacyDocumentsInferOutputsOnceAndExplicitEmptyOutputsStayEmpty)
{
	const auto ports = neurus::test::MakeRealPortSource();
	auto graph = RenderGraphTopology::Default(ports);
	auto record = neurus::project::ToRecord(graph);
	record.outputs.reset();
	std::vector<std::string> problems;
	auto restored = neurus::project::FromRecord(Deserialize(Serialize(record)), ports, problems);
	EXPECT_TRUE(problems.empty());
	EXPECT_EQ(restored.sceneOutputs, graph.sceneOutputs);
	ASSERT_TRUE(graph.RemoveNode(FindNode(graph, "fxaa")));
	record = neurus::project::ToRecord(graph);
	record.outputs.reset();
	auto compose = neurus::project::FromRecord(record, ports, problems);
	EXPECT_EQ(compose.sceneOutputs.at("color"), (neurus::GraphEndpoint{"compose", "outputComposed"}));
	EXPECT_TRUE(neurus::ValidateCpuGraph(compose, ports).empty());

	record.outputs.emplace();
	auto disconnected = neurus::project::FromRecord(Deserialize(Serialize(record)), ports, problems);
	EXPECT_TRUE(disconnected.sceneOutputs.empty());
	EXPECT_TRUE(HasDiagnostic(neurus::ValidateCpuGraph(disconnected, ports), "", "color"));
	auto empty = neurus::project::FromRecord({}, ports, problems);
	EXPECT_TRUE(neurus::ValidateCpuGraph(empty, ports).empty());
}

TEST(RenderGraphDocument, MissingCustomShaderRecoversRenamedInputsAndSelectedOutputByRole)
{
	const auto ports = neurus::test::MakeRealPortSource();
	auto graph = RenderGraphTopology::Default(ports);
	auto* compose = FindNode(graph, "compose");
	compose->data.shader = std::make_shared<TestShader>();
	auto contract = *ports.FindPassPorts("ComposePass");
	for (auto& port : contract.ports)
		port.name = port.direction == neurus::PortDirection::Input ? "customInput" : "customOutput";
	graph.sceneOutputs["color"] = {"compose", "outputComposed"};
	neurus::ReconcilePassPorts(graph, *compose, contract);
	const auto record = Deserialize(Serialize(neurus::project::ToRecord(graph)));
	EXPECT_EQ(graph.sceneOutputs.at("color").port, "customOutput");

	std::vector<std::string> problems;
	auto recovered = neurus::project::FromRecord(record, ports, problems);
	ASSERT_TRUE(problems.empty());
	EXPECT_TRUE(neurus::ValidateCpuGraph(recovered, ports).empty());
	EXPECT_EQ(recovered.sceneOutputs.at("color").port, "outputComposed");
	auto* fxaa = FindNode(recovered, "fxaa");
	ASSERT_TRUE(fxaa->inputs.front()->source);
	EXPECT_EQ(fxaa->inputs.front()->source->name, "outputComposed");
	EXPECT_EQ(FindNode(recovered, "compose")->data.shader, nullptr);
}

TEST(CpuGraphValidation, EditingThePublicProducerChangesReachabilityAndHash)
{
	const auto ports = neurus::test::MakeRealPortSource();
	const auto graph = RenderGraphTopology::Default(ports);
	auto bypass = neurus::CloneGraph(graph);
	bypass.sceneOutputs["color"] = {"compose", "outputComposed"};
	auto* fxaa = FindNode(bypass, "fxaa");
	bypass.Disconnect(fxaa->inputs.front()->source, fxaa->inputs.front().get());
	EXPECT_FALSE(bypass.ActiveNodes().contains("fxaa"));
	EXPECT_NE(bypass.Hash(), graph.Hash());
	EXPECT_TRUE(neurus::ValidateCpuGraph(bypass, ports).empty());

	bypass.sceneOutputs["color"] = {"geometry", "gObjectID"};
	EXPECT_TRUE(HasDiagnostic(neurus::ValidateCpuGraph(bypass, ports), "geometry", "incompatible scene output"));
	bypass.sceneOutputs["color"] = {"fxaa", "outputFXAA"};
	EXPECT_TRUE(HasDiagnostic(neurus::ValidateCpuGraph(bypass, ports), "fxaa", "not connected"));
}

TEST(CpuGraphValidation, PhysicalImageMustMatchItsPassAttachment)
{
	const auto ports = neurus::test::MakeRealPortSource();
	auto graph = RenderGraphTopology::Default(ports);
	auto* compose = FindNode(graph, "compose");
	compose->outputs.front()->data.tag->format = neurus::GraphFormat::R32Uint;
	EXPECT_TRUE(HasDiagnostic(neurus::ValidateCpuGraph(graph, ports), "compose", "incompatible"));
}
