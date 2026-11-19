/**
 * @file test_graph_component.cpp
 * @brief Project persistence for the render graph document.
 */

#include <gtest/gtest.h>

#include "shared/TestPassPorts.hpp"

#include "asset/components/GraphComponent.hpp"
#include "asset/components/ConfigComponent.hpp"
#include "asset/Project.hpp"
#include "core/RenderConfig.hpp"
#include "asset/data/RenderGraphData.hpp"

#include <cereal/archives/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string Save(const std::shared_ptr<neurus::RenderGraphData>& graph)
{
	std::ostringstream stream;
	{
		cereal::JSONOutputArchive archive(stream);
		// Keep the fixture a JSON object even when every optional component is absent.
		const bool fixture = true;
		archive(cereal::make_nvp("fixture", fixture));
		std::optional<neurus::project::RenderGraphRecord> pending;
		neurus::project::GraphComponent component(pending, graph);
		component.Save(archive);
	}
	return stream.str();
}

std::optional<neurus::project::RenderGraphRecord> Load(const std::string& text)
{
	std::optional<neurus::project::RenderGraphRecord> pending;
	std::istringstream stream(text);
	cereal::JSONInputArchive archive(stream);
	const std::shared_ptr<neurus::RenderGraphData> empty;
	neurus::project::GraphComponent component(pending, empty);
	component.Load(archive);
	return pending;
}

} // anonymous namespace

TEST(GraphComponent, RoundTrip)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	neurus::RenderGraphData graph = neurus::RenderGraphData::Default(ports);

	// A non-default tag and a removed edge must both survive the round trip.
	graph.nodes[0]->outputs[0]->data.tag->format = neurus::GraphFormat::Rgba8Unorm;
	neurus::RenderGraphData::NodeType* compose = graph.nodes[5].get();
	neurus::RenderGraphData::NodeType* fxaa = graph.nodes[6].get();
	graph.Disconnect(compose->outputs[0].get(), fxaa->inputs[0].get());

	const neurus::project::RenderGraphRecord expected = neurus::project::ToRecord(graph);
	const std::optional<neurus::project::RenderGraphRecord> restored = Load(Save(std::make_shared<neurus::RenderGraphData>(std::move(graph))));

	ASSERT_TRUE(restored.has_value());
	EXPECT_EQ(*restored, expected);
}

TEST(GraphComponent, MissingNodeLeavesNoPendingRecord)
{
	EXPECT_FALSE(Load("{}").has_value());
}

TEST(GraphComponent, CorruptNodeLeavesNoPendingRecord)
{
	EXPECT_FALSE(Load("{\"proj_render_graph\": 42}").has_value());
}

TEST(GraphComponent, NullGraphIsOmittedOnSave)
{
	const std::string saved = Save(nullptr);
	EXPECT_EQ(saved.find("proj_render_graph"), std::string::npos);
	EXPECT_FALSE(Load(saved).has_value());
}

TEST(GraphComponent, ExplicitEmptyGraphRemainsPresent)
{
	const auto graph = std::make_shared<neurus::RenderGraphData>();
	const std::string saved = Save(graph);
	EXPECT_NE(saved.find("proj_render_graph"), std::string::npos);
	const auto restored = Load(saved);
	ASSERT_TRUE(restored.has_value());
	EXPECT_TRUE(restored->nodes.empty());
	EXPECT_TRUE(restored->edges.empty());
	EXPECT_EQ(*restored, neurus::project::ToRecord(*graph));
}

TEST(GraphComponent, MissingOrCorruptGraphDiscardsPreviousPendingRecord)
{
	for (const std::string text : {"{}", "{\"proj_render_graph\": 42}"})
	{
		SCOPED_TRACE(text);
		std::optional<neurus::project::RenderGraphRecord> pending = neurus::project::RenderGraphRecord{};
		const std::shared_ptr<neurus::RenderGraphData> graph;
		std::istringstream stream(text);
		cereal::JSONInputArchive archive(stream);
		neurus::project::GraphComponent component(pending, graph);
		component.Load(archive);
		EXPECT_FALSE(pending.has_value());
	}
}

TEST(GraphComponent, NullShaderReferencesAreOmittedAndLoadAsDefaults)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	auto graph = std::make_shared<neurus::RenderGraphData>(neurus::RenderGraphData::Default(ports));
	const std::string saved = Save(graph);
	EXPECT_EQ(saved.find("\"shader\""), std::string::npos);
	const auto restored = Load(saved);
	ASSERT_TRUE(restored.has_value());
	ASSERT_EQ(restored->nodes.size(), graph->nodes.size());
	for (const auto& node : restored->nodes)
		EXPECT_EQ(node.shaderUid, 0);
}

TEST(GraphComponent, ExplicitShaderReferenceIsRetained)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	auto graph = std::make_shared<neurus::RenderGraphData>(neurus::RenderGraphData::Default(ports));
	graph->nodes.front()->data.shaderUid = 12345;
	const auto restored = Load(Save(graph));
	ASSERT_TRUE(restored.has_value());
	ASSERT_FALSE(restored->nodes.empty());
	EXPECT_EQ(restored->nodes.front().shaderUid, 12345);
}

TEST(GraphComponent, ProjectLoadRejectsMalformedNodeWithoutDiscardingLaterConfig)
{
	struct TemporaryProject
	{
		std::filesystem::path path = std::filesystem::temp_directory_path()
			/ ("neurus-malformed-graph-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
		~TemporaryProject()
		{
			std::error_code error;
			std::filesystem::remove(path, error);
		}
	} temporary;
	{
		std::ofstream file(temporary.path);
		ASSERT_TRUE(file.is_open());
		file << R"({"project":{"proj_render_graph":{"nodes":[42],"edges":[]},
			"proj_config":{"r_gamma":1.5,"r_shadow_bias":0.002}}})";
	}
	std::optional<neurus::project::RenderGraphRecord> pending = neurus::project::RenderGraphRecord{};
	const std::shared_ptr<neurus::RenderGraphData> graph;
	std::shared_ptr<neurus::RenderConfig> config;
	neurus::project::Project project;
	// Loading graph before config verifies malformed data cannot poison its sibling.
	project.Register<neurus::project::GraphComponent>(pending, graph);
	project.Register<neurus::project::ConfigComponent>(config);
	ASSERT_NO_THROW(project.Load(temporary.path.string()));
	EXPECT_FALSE(pending.has_value());
	EXPECT_FALSE(graph);
	ASSERT_TRUE(config);
	EXPECT_FLOAT_EQ(config->r_gamma, 1.5f);
	EXPECT_FLOAT_EQ(config->r_shadow_bias, 0.002f);
}

TEST(GraphComponent, LoadJsonGuardsNestedTypesAndKeepsNullInsteadOfAllocatingDefaults)
{
	const neurus::test::FakePortSource ports = neurus::test::MakeRealPortSource();
	auto original = std::make_shared<neurus::RenderGraphData>(neurus::RenderGraphData::Default(ports));
	const auto saved = nlohmann::ordered_json::parse(Save(original));
	std::vector<nlohmann::ordered_json> corrupt;
	corrupt.push_back(saved);
	corrupt.back()["proj_render_graph"]["nodes"][0]["pass"] = 42;
	corrupt.push_back(saved);
	corrupt.back()["proj_render_graph"]["nodes"][0]["shader"] = "bad";
	corrupt.push_back(saved);
	corrupt.back()["proj_render_graph"]["nodes"][0]["x"] = "bad";
	corrupt.push_back(saved);
	corrupt.back()["proj_render_graph"]["nodes"][0]["outputs"][0]["binding"] = -1;
	corrupt.push_back(saved);
	corrupt.back()["proj_render_graph"]["nodes"][0]["outputs"][0]["tag"]["nullopt"] = "bad";
	corrupt.push_back(saved);
	corrupt.back()["proj_render_graph"]["nodes"][0]["outputs"][0]["tag"]["data"]["extent"] = 42;
	corrupt.push_back(saved);
	corrupt.back()["proj_render_graph"]["edges"][0]["from"] = 42;
	for (const auto& document : corrupt)
	{
		SCOPED_TRACE(document.dump());
		std::optional<neurus::project::RenderGraphRecord> pending = neurus::project::RenderGraphRecord{};
		const std::shared_ptr<neurus::RenderGraphData> graph;
		neurus::project::GraphComponent component(pending, graph);
		EXPECT_NO_THROW(component.LoadJson(document.dump()));
		EXPECT_FALSE(pending.has_value());
		EXPECT_FALSE(graph);
	}
}

TEST(GraphComponent, LoadJsonAcceptsExplicitEmptyGraph)
{
	std::optional<neurus::project::RenderGraphRecord> pending;
	const std::shared_ptr<neurus::RenderGraphData> graph;
	neurus::project::GraphComponent component(pending, graph);
	component.LoadJson(R"({"proj_render_graph":{"nodes":[],"edges":[]}})");
	ASSERT_TRUE(pending.has_value());
	EXPECT_TRUE(pending->nodes.empty());
	EXPECT_TRUE(pending->edges.empty());
}
