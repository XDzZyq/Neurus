/**
 * @file test_graph_component.cpp
 * @brief Project persistence for the render graph document.
 */

#include <gtest/gtest.h>

#include "shared/TestPassPorts.hpp"

#include "asset/components/GraphComponent.hpp"
#include "asset/components/ConfigComponent.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/data/MeshData.hpp"
#include "core/ResourceManager.hpp"
#include "asset/Project.hpp"
#include "core/RenderConfig.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "shader/ComputeShader.hpp"

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
		std::optional<neurus::project::GraphSelection> pending;
		neurus::project::GraphComponent component(pending, graph);
		component.Save(archive);
	}
	return stream.str();
}

std::optional<neurus::project::GraphSelection> Load(const std::string& text)
{
	std::optional<neurus::project::GraphSelection> pending;
	std::istringstream stream(text);
	cereal::JSONInputArchive archive(stream);
	const std::shared_ptr<neurus::RenderGraphData> empty;
	neurus::project::GraphComponent component(pending, empty);
	component.Load(archive);
	return pending;
}

nlohmann::ordered_json SaveInline(const neurus::RenderGraphTopology& topology)
{
	std::ostringstream stream;
	{
		cereal::JSONOutputArchive archive(stream);
		archive(cereal::make_nvp("proj_render_graph", neurus::project::ToRecord(topology)));
	}
	return nlohmann::ordered_json::parse(stream.str());
}

nlohmann::ordered_json SavePool(neurus::ResourceManager& pool)
{
	std::ostringstream stream;
	{
		cereal::JSONOutputArchive archive(stream);
		neurus::project::ResourceComponent(pool).Save(archive);
	}
	return nlohmann::ordered_json::parse(stream.str());
}

} // anonymous namespace

TEST(GraphComponent, ActiveUidSelectsOnePooledPayload)
{
	const auto ports = neurus::test::MakeRealPortSource();
	neurus::ResourceManager pool;
	auto graph = pool.Load<neurus::RenderGraphData>(neurus::RenderGraphTopology::Default(ports));
	graph->nodes.front()->outputs.front()->data.tag->format = neurus::GraphFormat::Rgba8Unorm;
	graph->Disconnect(graph->nodes[5]->outputs.front().get(), graph->nodes[6]->inputs.front().get());
	const auto expected = neurus::project::ToRecord(*graph);

	const auto project = nlohmann::ordered_json::parse(Save(graph));
	const auto& active = project.at("proj_render_graph");
	ASSERT_EQ(active.size(), 1u);
	ASSERT_EQ(active.at("uid"), graph->GetObjectID());
	const auto pending = Load(project.dump());
	ASSERT_TRUE(pending && std::holds_alternative<int>(*pending));

	const auto savedPool = SavePool(pool);
	const auto& entries = savedPool.at("m_resources").at("resources_");
	ASSERT_EQ(entries.size(), 1u);
	const auto& payload = entries[0].at("value").at("ptr_wrapper").at("data").at("topology");
	EXPECT_EQ(payload.at("nodes").size(), expected.nodes.size());
	EXPECT_EQ(payload.at("edges").size(), expected.edges.size());

	neurus::ResourceManager loaded;
	neurus::project::ResourceComponent(loaded).LoadJson(savedPool.dump());
	auto restored = loaded.Get<neurus::RenderGraphData>(std::get<int>(*pending));
	ASSERT_TRUE(restored);
	std::vector<std::string> problems;
	ASSERT_TRUE(restored->Resolve(ports, loaded, problems));
	EXPECT_EQ(neurus::project::ToRecord(*restored), expected);
}

TEST(GraphComponent, CorruptFirstGraphDoesNotDiscardSiblingResources)
{
	neurus::ResourceManager pool;
	pool.Load<neurus::RenderGraphData>();
	pool.Load<neurus::RenderGraphData>();
	auto mesh = pool.Load<neurus::MeshData>();

	auto saved = SavePool(pool);
	int rejected = -1;
	for (auto& entry : saved.at("m_resources").at("resources_"))
	{
		auto& value = entry.at("value");
		if (value.value("polymorphic_name", "") == "neurus::RenderGraphData")
		{
			rejected = entry.at("key").get<int>();
			value.at("ptr_wrapper").at("data").at("topology")["edges"] = 42;
			break;
		}
	}
	ASSERT_GE(rejected, 0);
	neurus::ResourceManager restored;
	neurus::project::ResourceComponent(restored).LoadJson(saved.dump());
	EXPECT_FALSE(restored.Get<neurus::RenderGraphData>(rejected));
	EXPECT_TRUE(restored.Get<neurus::MeshData>(mesh->GetObjectID()));
	EXPECT_EQ(restored.Size(), 2u);
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
	ASSERT_TRUE(std::holds_alternative<int>(*restored));
	EXPECT_EQ(std::get<int>(*restored), graph->GetObjectID());
}

TEST(GraphComponent, MissingOrCorruptGraphDiscardsPreviousPendingRecord)
{
	for (const std::string text : {"{}", "{\"proj_render_graph\": 42}"})
	{
		SCOPED_TRACE(text);
		std::optional<neurus::project::GraphSelection> pending = neurus::project::RenderGraphRecord{};
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
	const auto graph = neurus::RenderGraphTopology::Default(ports);
	const std::string saved = SaveInline(graph).dump();
	EXPECT_EQ(saved.find("\"shader\""), std::string::npos);
	const auto restored = Load(saved);
	ASSERT_TRUE(restored && std::holds_alternative<neurus::project::RenderGraphRecord>(*restored));
	const auto& record = std::get<neurus::project::RenderGraphRecord>(*restored);
	ASSERT_EQ(record.nodes.size(), graph.nodes.size());
	for (const auto& node : record.nodes)
		EXPECT_EQ(node.shaderUid, 0);
}

TEST(GraphComponent, PooledGraphRestoresSharedShaderReferences)
{
	const auto ports = neurus::test::MakeRealPortSource();
	neurus::ResourceManager pool;
	auto graph = pool.Load<neurus::RenderGraphData>(neurus::RenderGraphTopology::Default(ports));
	auto shader = pool.Load<neurus::ComputeShader>("Custom",
		neurus::ShaderData::ResolveSourcePath("res/shaders/compute/compose.comp.glsl"));
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	graph->nodes[5]->data.shader = shader;
	graph->nodes[6]->data.shader = shader;

	neurus::ResourceManager restored;
	neurus::project::ResourceComponent(restored).LoadJson(SavePool(pool).dump());
	ASSERT_EQ(restored.Size(), 2u);
	const auto loaded = restored.Get<neurus::RenderGraphData>(graph->GetObjectID());
	ASSERT_TRUE(loaded);
	std::vector<std::string> problems;
	ASSERT_TRUE(loaded->Resolve(ports, restored, problems));
	const auto loadedShader = restored.Get<neurus::ComputeShader>(shader->GetObjectID());
	ASSERT_TRUE(loadedShader);
	ASSERT_EQ(loaded->nodes.size(), 7u);
	EXPECT_EQ(loaded->nodes[5]->data.shader, loadedShader);
	EXPECT_EQ(loaded->nodes[6]->data.shader, loadedShader);
	EXPECT_FALSE(loaded->nodes[4]->data.shader);
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
	std::optional<neurus::project::GraphSelection> pending = neurus::project::RenderGraphRecord{};
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
	const auto saved = SaveInline(neurus::RenderGraphTopology::Default(ports));
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
		std::optional<neurus::project::GraphSelection> pending = neurus::project::RenderGraphRecord{};
		const std::shared_ptr<neurus::RenderGraphData> graph;
		neurus::project::GraphComponent component(pending, graph);
		EXPECT_NO_THROW(component.LoadJson(document.dump()));
		EXPECT_FALSE(pending.has_value());
		EXPECT_FALSE(graph);
	}
}

TEST(GraphComponent, LoadJsonAcceptsExplicitEmptyGraph)
{
	std::optional<neurus::project::GraphSelection> pending;
	const std::shared_ptr<neurus::RenderGraphData> graph;
	neurus::project::GraphComponent component(pending, graph);
	component.LoadJson(R"({"proj_render_graph":{"nodes":[],"edges":[]}})");
	ASSERT_TRUE(pending.has_value());
	ASSERT_TRUE(std::holds_alternative<neurus::project::RenderGraphRecord>(*pending));
	const auto& record = std::get<neurus::project::RenderGraphRecord>(*pending);
	EXPECT_TRUE(record.nodes.empty());
	EXPECT_TRUE(record.edges.empty());
}
