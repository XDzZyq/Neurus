/**
 * @file test_graph_component.cpp
 * @brief Project persistence for the render graph document.
 */

#include <gtest/gtest.h>

#include "asset/components/GraphComponent.hpp"
#include "asset/components/ConfigComponent.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/data/render_graph/RenderGraphRecord.hpp"
#include "asset/data/MeshData.hpp"
#include "asset/ResourceManager.hpp"
#include "asset/Project.hpp"
#include "core/RenderConfig.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "shader/ComputeShader.hpp"

#include <cereal/archives/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

class GraphComponent : public ::testing::Test
{
protected:
	neurus::BuiltInRenderData catalog;
	neurus::ResourceManager resources;
	neurus::ResourceRef<neurus::RenderGraphData> selected;
	std::vector<std::string> diagnostics;

	std::string Save(neurus::ResourceRef<neurus::RenderGraphData> graph)
	{
		std::ostringstream stream;
		{
			cereal::JSONOutputArchive archive(stream);
			const bool fixture = true;
			archive(cereal::make_nvp("fixture", fixture));
			neurus::project::GraphComponent(graph, resources, diagnostics).Save(archive);
		}
		return stream.str();
	}

	void Load(const std::string& text)
	{
		neurus::project::GraphComponent(selected, resources, diagnostics).LoadJson(text);
	}

	nlohmann::ordered_json SavePool(neurus::ResourceManager& pool)
	{
		std::ostringstream stream;
		{
			cereal::JSONOutputArchive archive(stream);
			neurus::project::ResourceComponent(pool, neurus::project::MakeGraphReadServices(catalog)).Save(archive);
		}
		return nlohmann::ordered_json::parse(stream.str());
	}
};

} // anonymous namespace

TEST_F(GraphComponent, ActiveUidSelectsOnePooledPayload)
{
	const auto& ports = catalog.Ports();
	neurus::ResourceManager pool;
	auto graph = pool.Load<neurus::RenderGraphData>(ports);
	graph->nodes.front()->data.x = 16.0f;
	graph->sceneOutputs["color"] = {"compose", "outputComposed"};
	const auto expected = neurus::project::ToRecord(*graph);

	const auto project = nlohmann::ordered_json::parse(Save(graph));
	const auto& active = project.at("proj_render_graph");
	ASSERT_EQ(active.size(), 1u);
	ASSERT_EQ(active.at("uid"), graph->RequireObjectID());

	const auto savedPool = SavePool(pool);
	const auto& entries = savedPool.at("m_resources").at("resources_");
	ASSERT_EQ(entries.size(), 1u);
	const auto& payload = entries[0].at("data").at("topology");
	EXPECT_EQ(payload.at("nodes").size(), expected.nodes.size());
	EXPECT_EQ(payload.at("edges").size(), expected.edges.size());

	neurus::project::ResourceComponent(resources, neurus::project::MakeGraphReadServices(catalog)).LoadJson(savedPool.dump());
	Load(project.dump());
	ASSERT_TRUE(selected);
	EXPECT_EQ(selected, resources.Get<neurus::RenderGraphData>(graph->RequireObjectID()));
	EXPECT_EQ(neurus::project::ToRecord(*selected), expected);

	// A save after editing must read the live topology, not the original record.
	selected->nodes.front()->data.x = 32.0f;
	const auto edited = SavePool(resources);
	EXPECT_EQ(edited.at("m_resources").at("resources_")[0]
	              .at("data").at("topology").at("nodes")[0].at("x"), 32.0f);
}

TEST_F(GraphComponent, CorruptFirstGraphDoesNotDiscardSiblingResources)
{
	neurus::ResourceManager pool;
	pool.Load<neurus::RenderGraphData>();
	pool.Load<neurus::RenderGraphData>();
	auto mesh = pool.Load<neurus::MeshData>();

	auto saved = SavePool(pool);
	int rejected = -1;
	for (auto& entry : saved.at("m_resources").at("resources_"))
	{
		if (entry.at("type") == "neurus::RenderGraphData")
		{
			rejected = entry.at("key").get<int>();
			entry.at("data").at("topology")["edges"] = 42;
			break;
		}
	}
	ASSERT_GE(rejected, 0);
	neurus::ResourceManager restored;
	neurus::project::ResourceComponent(restored, neurus::project::MakeGraphReadServices(catalog)).LoadJson(saved.dump());
	EXPECT_FALSE(restored.Get<neurus::RenderGraphData>(rejected));
	EXPECT_TRUE(restored.Get<neurus::MeshData>(mesh->RequireObjectID()));
	EXPECT_EQ(restored.Size(), 2u);
}

TEST_F(GraphComponent, NullGraphIsOmittedOnSave)
{
	const std::string saved = Save(nullptr);
	EXPECT_EQ(saved.find("proj_render_graph"), std::string::npos);
	Load(saved);
	EXPECT_FALSE(selected);
}

TEST_F(GraphComponent, ExplicitEmptyGraphRemainsPresent)
{
	const auto graph = resources.Load<neurus::RenderGraphData>();
	const std::string saved = Save(graph);
	EXPECT_NE(saved.find("proj_render_graph"), std::string::npos);
	Load(saved);
	EXPECT_EQ(selected, graph);
}

TEST_F(GraphComponent, MissingOrCorruptGraphClearsPreviousSelection)
{
	for (const std::string text : {"{}", "{\"proj_render_graph\": 42}"})
	{
		SCOPED_TRACE(text);
		selected = resources.Load<neurus::RenderGraphData>();
		Load(text);
		EXPECT_FALSE(selected);
	}
}

TEST_F(GraphComponent, NullShaderReferencesAreOmittedAndLoadAsDefaults)
{
	const auto& ports = catalog.Ports();
	neurus::ResourceManager source;
	const auto graph = source.Load<neurus::RenderGraphData>(ports);
	const std::string saved = SavePool(source).dump();
	EXPECT_EQ(saved.find("\"shader\""), std::string::npos);
	neurus::project::ResourceComponent(resources, neurus::project::MakeGraphReadServices(catalog)).LoadJson(saved);
	Load(Save(graph));
	ASSERT_TRUE(selected);
	ASSERT_EQ(selected->nodes.size(), graph->nodes.size());
	for (const auto& node : selected->nodes)
		EXPECT_FALSE(node->data.shader);
}

TEST_F(GraphComponent, InactiveGraphResolvesShadersStoredLaterInPool)
{
	const auto& ports = catalog.Ports();
	neurus::ResourceManager pool;
	auto graph = pool.Load<neurus::RenderGraphData>(ports);
	auto shader = pool.Load<neurus::ComputeShader>("Custom",
		neurus::ShaderData::ResolveSourcePath("res/shaders/compute/compose.comp.glsl"));
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	auto* compose = graph->nodes[5].get();
	compose->data.shader = shader;
	auto* second = graph->AddNode("compose_again", compose->data);
	for (const auto& input : compose->inputs)
		second->AddInput(input->name)->data = input->data;
	for (const auto& output : compose->outputs)
		second->AddOutput(output->name)->data = output->data;
	ASSERT_TRUE(graph->Connect(compose->outputs.front().get(), second->inputs.front().get()));
	graph->sceneOutputs["color"] = {second->name, second->outputs.front()->name};

	neurus::ResourceManager restored;
	auto saved = SavePool(pool);
	auto& entries = saved.at("m_resources").at("resources_");
	ASSERT_EQ(entries.size(), 2u);
	if (entries[0].at("key") != graph->RequireObjectID())
		std::swap(entries[0], entries[1]);
	{
		const neurus::BuiltInRenderData temporaryCatalog;
		neurus::project::ResourceComponent(restored, neurus::project::MakeGraphReadServices(temporaryCatalog)).LoadJson(saved.dump());
	}
	ASSERT_EQ(restored.Size(), 2u);
	const auto loaded = restored.Get<neurus::RenderGraphData>(graph->RequireObjectID());
	ASSERT_TRUE(loaded);
	const auto loadedShader = restored.Get<neurus::ComputeShader>(shader->RequireObjectID());
	ASSERT_TRUE(loadedShader);
	ASSERT_EQ(loaded->nodes.size(), 8u);
	EXPECT_EQ(loaded->nodes[5]->data.shader.get(), loadedShader.get());
	EXPECT_EQ(loaded->nodes[7]->data.shader.get(), loadedShader.get());
	EXPECT_TRUE(neurus::ValidateCpuGraph(*loaded, ports).empty());
	EXPECT_FALSE(loaded->nodes[4]->data.shader);
}

TEST_F(GraphComponent, StandaloneArchiveRestoresGraphAndShaderTogether)
{
	neurus::ResourceManager source;
	const auto graph = source.Load<neurus::RenderGraphData>(catalog.Ports());
	const auto shader = catalog.CreatePassShader(graph->nodes.at(5)->data.pass, "ArchivedCompose");
	source.Register(shader);
	graph->nodes[5]->data.shader = shader;

	neurus::ResourceManager restored;
	std::istringstream stream(SavePool(source).dump());
	cereal::JSONInputArchive archive(stream);
	neurus::project::ResourceComponent(restored, neurus::project::MakeGraphReadServices(catalog)).Load(archive);

	const auto loadedGraph = restored.Get<neurus::RenderGraphData>(graph->RequireObjectID());
	ASSERT_TRUE(loadedGraph);
	ASSERT_EQ(loadedGraph->nodes.size(), graph->nodes.size());
	const auto loadedShader = restored.Get<neurus::ShaderData>(shader->RequireObjectID());
	ASSERT_TRUE(loadedShader);
	EXPECT_EQ(loadedGraph->nodes[5]->data.shader.get(), loadedShader.get());
	EXPECT_EQ(neurus::project::ToRecord(*loadedGraph), neurus::project::ToRecord(*graph));
}

TEST_F(GraphComponent, ProjectLoadRejectsMalformedSelectionWithoutDiscardingLaterConfig)
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
		file << R"({"project":{"proj_render_graph":{"uid":"bad"},
			"proj_config":{"r_gamma":1.5,"r_shadow_bias":0.002}}})";
	}
	selected = resources.Load<neurus::RenderGraphData>();
	std::shared_ptr<neurus::RenderConfig> config;
	neurus::project::Project project;
	// Loading graph before config verifies malformed data cannot poison its sibling.
	project.Register<neurus::project::GraphComponent>(selected, resources, diagnostics);
	project.Register<neurus::project::ConfigComponent>(config);
	ASSERT_NO_THROW(project.Load(temporary.path.string()));
	EXPECT_FALSE(selected);
	ASSERT_TRUE(config);
	EXPECT_FLOAT_EQ(config->r_gamma, 1.5f);
	EXPECT_FLOAT_EQ(config->r_shadow_bias, 0.002f);
}

TEST_F(GraphComponent, PoolReaderRejectsMalformedTopology)
{
	neurus::ResourceManager source;
	const auto graph = source.Load<neurus::RenderGraphData>(catalog.Ports());
	const auto saved = SavePool(source);
	auto topology = [](auto& pool) -> auto& {
		return pool.at("m_resources").at("resources_")[0].at("data").at("topology");
	};
	std::vector<nlohmann::ordered_json> corrupt;
	corrupt.push_back(saved);
	topology(corrupt.back())["nodes"][0]["pass"] = 42;
	corrupt.push_back(saved);
	topology(corrupt.back())["nodes"][0]["shader"] = "bad";
	corrupt.push_back(saved);
	topology(corrupt.back())["nodes"][0]["x"] = "bad";
	corrupt.push_back(saved);
	topology(corrupt.back())["nodes"][0]["outputs"][0]["binding"] = -1;
	corrupt.push_back(saved);
	topology(corrupt.back())["nodes"][0]["outputs"][0]["tag"]["nullopt"] = "bad";
	corrupt.push_back(saved);
	topology(corrupt.back())["nodes"][0]["outputs"][0]["tag"]["data"]["extent"] = 42;
	corrupt.push_back(saved);
	topology(corrupt.back())["edges"][0]["from"] = 42;
	for (const auto& document : corrupt)
	{
		SCOPED_TRACE(document.dump());
		neurus::project::ResourceComponent(resources, neurus::project::MakeGraphReadServices(catalog)).LoadJson(document.dump());
		EXPECT_FALSE(resources.Get<neurus::RenderGraphData>(graph->RequireObjectID()));
	}
}

TEST_F(GraphComponent, InlineTopologyIsNotAnActiveUid)
{
	Load(R"({"proj_render_graph":{"nodes":[],"edges":[]}})");
	EXPECT_FALSE(selected);
	EXPECT_EQ(resources.Size(), 0u);
}
