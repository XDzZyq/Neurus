/**
 * @file test_editor_render_graph.cpp
 * @brief Nullable project render state stays separate from cached CPU defaults.
 */

#include <gtest/gtest.h>

#include "editor/Editor.hpp"
#include "core/ResourceManager.hpp"
#include "editor/RenderManager.hpp"
#include "asset/components/GraphComponent.hpp"
#include "asset/components/ConfigComponent.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "shader/ComputeShader.hpp"
#include "shader/RenderShader.hpp"
#include "scene/Mesh.hpp"
#include "editor/events/ShaderEvents.hpp"
#include "render_api/RenderRequestSink.hpp"
#include "render_api/RenderRequests.hpp"

#include <algorithm>
#include <memory>
#include <sstream>
#include <variant>
#include <utility>
#include <vector>

namespace {

class RecordingSink : public neurus::RenderRequestSink
{
public:
	void Post(neurus::RenderRequest&& request) override
	{
		posted.push_back(std::move(request));
	}

	const neurus::CompileRenderGraphRequest* LastGraphRequest() const
	{
		if (posted.empty())
			return nullptr;
		return std::get_if<neurus::CompileRenderGraphRequest>(&posted.back());
	}

	std::vector<neurus::RenderRequest> posted;
};

neurus::RenderConfig NonDefaultConfig()
{
	neurus::RenderConfig config;
	config.r_pipeline = neurus::RenderPipeLine::Forward;
	config.r_aa = neurus::AAAlg::FXAA;
	config.r_ao = neurus::AOAlg::None;
	config.r_shadow = neurus::ShadowAlg::VSSM;
	config.r_ssr = neurus::SSRAlg::RayMarching;
	config.r_gamma = 1.7f;
	config.r_shadow_bias = 0.002f;
	config.r_sampling_mode = 1;
	config.r_ao_ksize = 24;
	config.r_ao_radius = 0.75f;
	config.r_sample_pf = 17;
	config.r_sampling_count = 23;
	config.r_transparent = true;
	config.r_debug_draw = false;
	config.r_fxaa_subpix = 0.5f;
	config.r_fxaa_edge_threshold = 0.2f;
	config.r_fxaa_edge_threshold_min = 0.04f;
	return config;
}

} // anonymous namespace

TEST(EditorRenderGraph, NewScenePublishesNullWithoutPoolingDefaults)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.EditRenderConfig() = NonDefaultConfig();
	editor.NewScene();
	EXPECT_EQ(editor.GetRenderConfig(), neurus::RenderConfig{});
	EXPECT_FALSE(editor.GetRenderConfigData());
	EXPECT_FALSE(editor.GetRenderManager().GraphData());
	ASSERT_EQ(editor.GetRenderGraph().nodes.size(), 7u);

	editor.GetResourceManager().ForEachUID([](const std::shared_ptr<neurus::UID>& resource)
	{
		EXPECT_EQ(std::dynamic_pointer_cast<neurus::ShaderData>(resource), nullptr);
	});
	EXPECT_TRUE(editor.GetRenderManager().Problems().empty());
	EXPECT_FALSE(editor.IsDirty());
	const auto* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_FALSE(request->graph);
}

TEST(EditorRenderGraph, FinishLoadAppliesThePendingRecord)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();

	neurus::project::RenderGraphRecord record = neurus::project::ToRecord(editor.GetRenderGraph());
	record.nodes.pop_back();
	record.edges.erase(
		std::remove_if(record.edges.begin(), record.edges.end(),
			[](const neurus::project::RenderGraphEdgeRecord& edge)
			{
				return edge.from == "fxaa" || edge.to == "fxaa";
			}),
		record.edges.end());
	editor.GetRenderManager().PendingRecord() = record;
	const neurus::RenderConfig expected = NonDefaultConfig();
	editor.EditRenderConfig() = expected;

	editor.FinishLoad();

	EXPECT_EQ(editor.GetRenderConfig(), expected);
	EXPECT_EQ(editor.BuildSnapshot().Settings(), expected);

	EXPECT_EQ(editor.GetRenderGraph().nodes.size(), 6u);
	EXPECT_EQ(editor.GetRenderGraph().nodes[5]->name, "compose");
	EXPECT_FALSE(editor.IsDirty());

	const neurus::CompileRenderGraphRequest* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_EQ(neurus::project::ToRecord(*request->graph), neurus::project::ToRecord(editor.GetRenderGraph()));
}

TEST(EditorRenderGraph, SetPostsAndMarksDirty)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();
	editor.ClearDirty();

	neurus::RenderGraphData graph = neurus::CloneGraph(editor.GetRenderGraph());
	graph.RemoveNode(graph.nodes.back().get());
	editor.SetRenderGraph(std::move(graph));

	EXPECT_TRUE(editor.IsDirty());
	EXPECT_EQ(editor.GetRenderGraph().nodes.size(), 6u);
	EXPECT_EQ(editor.GetRenderGraph().nodes.back()->name, "compose");

	const neurus::CompileRenderGraphRequest* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_EQ(neurus::project::ToRecord(*request->graph), neurus::project::ToRecord(editor.GetRenderGraph()));
}

TEST(EditorRenderGraph, FinishLoadWithoutGraphPreservesDeserializedConfig)
{
	auto expectedData = std::make_shared<neurus::RenderConfig>(NonDefaultConfig());
	const neurus::RenderConfig expected = *expectedData;
	std::stringstream serialized;
	{
		cereal::JSONOutputArchive archive(serialized);
		neurus::project::ConfigComponent config(expectedData);
		config.Save(archive);
	}

	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();
	editor.EditRenderConfig() = expected;
	editor.BeginLoad();
	ASSERT_EQ(editor.GetRenderConfig(), neurus::RenderConfig{});
	ASSERT_FALSE(editor.GetRenderConfigData());
	{
		cereal::JSONInputArchive archive(serialized);
		neurus::project::ConfigComponent config(editor.GetRenderConfigData());
		config.Load(archive);
		neurus::RenderManager& render = editor.GetRenderManager();
		neurus::project::GraphComponent graph(render.PendingRecord(), render.GraphData());
		graph.Load(archive);
	}
	ASSERT_EQ(editor.GetRenderConfig(), expected);
	ASSERT_FALSE(editor.GetRenderManager().PendingRecord().has_value());

	editor.FinishLoad();

	EXPECT_EQ(editor.GetRenderConfig(), expected);
	EXPECT_EQ(editor.BuildSnapshot().Settings(), expected);
	EXPECT_EQ(editor.GetRenderGraph().nodes.size(), 7u);
	EXPECT_NE(sink.LastGraphRequest(), nullptr);
	EXPECT_FALSE(editor.IsDirty());
}

TEST(EditorRenderGraph, ResetConfigPreservesGraphAndDoesNotPublish)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();
	neurus::RenderGraphData graph = neurus::CloneGraph(editor.GetRenderGraph());
	graph.RemoveNode(graph.nodes.back().get());
	editor.SetRenderGraph(std::move(graph));
	neurus::RenderManager& render = editor.GetRenderManager();
	render.EditConfig() = NonDefaultConfig();
	const auto expected = neurus::project::ToRecord(render.Graph());
	render.PendingRecord() = expected;
	sink.posted.clear();

	render.ResetConfig();

	EXPECT_EQ(render.Config(), neurus::RenderConfig{});
	EXPECT_FALSE(render.ConfigData());
	EXPECT_TRUE(render.GraphData());
	EXPECT_EQ(neurus::project::ToRecord(render.Graph()), expected);
	ASSERT_TRUE(render.PendingRecord().has_value());
	EXPECT_EQ(*render.PendingRecord(), expected);
	EXPECT_TRUE(sink.posted.empty());
}

TEST(EditorRenderGraph, ResetGraphPreservesConfig)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();
	neurus::RenderGraphData graph = neurus::CloneGraph(editor.GetRenderGraph());
	graph.RemoveNode(graph.nodes.back().get());
	editor.SetRenderGraph(std::move(graph));
	const neurus::RenderConfig expected = NonDefaultConfig();
	editor.EditRenderConfig() = expected;
	sink.posted.clear();

	editor.GetRenderManager().ResetGraph();

	EXPECT_EQ(editor.GetRenderConfig(), expected);
	EXPECT_EQ(editor.BuildSnapshot().Settings(), expected);
	EXPECT_EQ(editor.GetRenderGraph().nodes.size(), 7u);
	EXPECT_FALSE(editor.GetRenderManager().GraphData());
	EXPECT_TRUE(editor.GetRenderConfigData());
	const neurus::CompileRenderGraphRequest* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_FALSE(request->graph);
}

namespace {

std::string SaveOptionalRenderState(neurus::Editor& editor)
{
	std::ostringstream stream;
	{
		cereal::JSONOutputArchive archive(stream);
		// Keep the fixture a JSON object even when every optional component is absent.
		const bool fixture = true;
		archive(cereal::make_nvp("fixture", fixture));
		neurus::project::ConfigComponent config(editor.GetRenderConfigData());
		config.Save(archive);
		auto& render = editor.GetRenderManager();
		neurus::project::GraphComponent graph(render.PendingRecord(), render.GraphData());
		graph.Save(archive);
	}
	return stream.str();
}

void LoadOptionalRenderState(neurus::Editor& editor, const std::string& text)
{
	editor.BeginLoad();
	std::istringstream stream(text);
	{
		cereal::JSONInputArchive archive(stream);
		neurus::project::ConfigComponent config(editor.GetRenderConfigData());
		config.Load(archive);
		auto& render = editor.GetRenderManager();
		neurus::project::GraphComponent graph(render.PendingRecord(), render.GraphData());
		graph.Load(archive);
	}
	editor.FinishLoad();
}

} // anonymous namespace

TEST(EditorRenderGraph, DefaultReadsSnapshotsAndPublishDoNotMaterializeProjectData)
{
	RecordingSink sink;
	const auto defaults = std::make_shared<const neurus::BuiltInRenderData>();
	neurus::Editor editor(&sink, defaults);
	editor.NewScene();
	const auto* graph = defaults->Graph().get();
	EXPECT_EQ(&editor.GetRenderGraph(), graph);

	neurus::Editor sharingDefaults(nullptr, defaults);
	EXPECT_EQ(&sharingDefaults.GetRenderGraph(), graph);
	neurus::Editor standalone(nullptr);
	EXPECT_NE(&standalone.GetRenderGraph(), graph);
	EXPECT_EQ(standalone.GetRenderGraph().Hash(), graph->Hash());

	const auto shader = defaults->ShaderFor("GeometryPass");
	ASSERT_TRUE(shader);
	EXPECT_EQ(defaults->ShaderFor("GeometryPass"), shader);
	EXPECT_EQ(editor.BuildSnapshot().Settings(), neurus::RenderConfig{});
	editor.GetRenderManager().Publish();
	EXPECT_FALSE(editor.GetRenderConfigData());
	EXPECT_FALSE(editor.GetRenderManager().GraphData());
	editor.GetResourceManager().ForEachUID([](const std::shared_ptr<neurus::UID>& resource)
	{
		EXPECT_EQ(std::dynamic_pointer_cast<neurus::ShaderData>(resource), nullptr);
	});
	const std::string saved = SaveOptionalRenderState(editor);
	EXPECT_EQ(saved.find("proj_config"), std::string::npos);
	EXPECT_EQ(saved.find("proj_render_graph"), std::string::npos);
	LoadOptionalRenderState(editor, saved);
	EXPECT_FALSE(editor.GetRenderConfigData());
	EXPECT_FALSE(editor.GetRenderManager().GraphData());
	EXPECT_EQ(&editor.GetRenderGraph(), graph);
}

TEST(EditorRenderGraph, PresentPartialConfigAndAbsentGraphRoundTripIndependently)
{
	neurus::Editor editor(nullptr);
	LoadOptionalRenderState(editor, "{\"proj_config\": {\"r_gamma\": 1.5}}");
	ASSERT_TRUE(editor.GetRenderConfigData());
	EXPECT_FLOAT_EQ(editor.GetRenderConfig().r_gamma, 1.5f);
	EXPECT_FLOAT_EQ(editor.GetRenderConfig().r_shadow_bias, neurus::RenderConfig{}.r_shadow_bias);
	EXPECT_FALSE(editor.GetRenderManager().GraphData());
	const std::string saved = SaveOptionalRenderState(editor);
	EXPECT_NE(saved.find("proj_config"), std::string::npos);
	EXPECT_EQ(saved.find("proj_render_graph"), std::string::npos);
	LoadOptionalRenderState(editor, saved);
	EXPECT_TRUE(editor.GetRenderConfigData());
	EXPECT_FALSE(editor.GetRenderManager().GraphData());
	EXPECT_FLOAT_EQ(editor.GetRenderConfig().r_gamma, 1.5f);
}

TEST(EditorRenderGraph, MalformedGraphDoesNotDiscardPresentConfigOrSerializeDefaults)
{
	neurus::Editor editor(nullptr);
	LoadOptionalRenderState(editor, "{\"proj_config\": {\"r_shadow_bias\": 0.002}, \"proj_render_graph\": 42}");
	ASSERT_TRUE(editor.GetRenderConfigData());
	EXPECT_FLOAT_EQ(editor.GetRenderConfig().r_shadow_bias, 0.002f);
	EXPECT_FALSE(editor.GetRenderManager().GraphData());
	const std::string saved = SaveOptionalRenderState(editor);
	EXPECT_NE(saved.find("proj_config"), std::string::npos);
	EXPECT_EQ(saved.find("proj_render_graph"), std::string::npos);
}

TEST(EditorRenderGraph, ExplicitEmptyGraphRoundTripsWithoutBecomingDefault)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();
	editor.SetRenderGraph(neurus::RenderGraphData{});
	ASSERT_TRUE(editor.GetRenderManager().GraphData());
	EXPECT_TRUE(editor.GetRenderGraph().nodes.empty());
	const auto* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	ASSERT_TRUE(request->graph);
	EXPECT_TRUE(request->graph->nodes.empty());
	const std::string saved = SaveOptionalRenderState(editor);
	EXPECT_NE(saved.find("proj_render_graph"), std::string::npos);
	EXPECT_EQ(saved.find("proj_config"), std::string::npos);
	LoadOptionalRenderState(editor, saved);
	ASSERT_TRUE(editor.GetRenderManager().GraphData());
	EXPECT_TRUE(editor.GetRenderGraph().nodes.empty());
	EXPECT_FALSE(editor.GetRenderConfigData());
}

TEST(EditorRenderGraph, PublicationRetainsShaderIdentityAndUploadsBeforeMapping)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();
	auto shader = editor.GetResourceManager().Load<neurus::ComputeShader>(
		"CustomCompose", "res/shaders/compute/compose.comp.glsl");
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	auto graph = neurus::CloneGraph(editor.GetRenderGraph());
	graph.nodes[5]->data.shader = shader;
	sink.posted.clear();
	editor.SetRenderGraph(std::move(graph));
	ASSERT_EQ(sink.posted.size(), 2u);
	const auto* firstUpload = std::get_if<neurus::UploadShaderRequest>(&sink.posted[0]);
	ASSERT_NE(firstUpload, nullptr);
	const auto firstBytes = firstUpload->stages.at(neurus::ShaderType::COMPUTE).spv;
	EXPECT_EQ(firstUpload->shaderId, shader->GetObjectID());
	EXPECT_EQ(firstUpload->primaryType, neurus::ShaderType::COMPUTE);
	const auto first = sink.LastGraphRequest()->graph;
	ASSERT_TRUE(first);
	EXPECT_EQ(first->nodes[5]->data.shader, shader);
	EXPECT_NE(first.get(), editor.GetRenderManager().GraphData().get());
	const auto firstHash = first->Hash();

	shader->GetStage(neurus::ShaderType::COMPUTE).spv.back() ^= 1;
	EXPECT_EQ(firstUpload->stages.at(neurus::ShaderType::COMPUTE).spv, firstBytes);
	EXPECT_EQ(first->Hash(), firstHash);
	editor.GetRenderManager().Publish();
	ASSERT_EQ(sink.posted.size(), 4u);
	const auto* secondUpload = std::get_if<neurus::UploadShaderRequest>(&sink.posted[2]);
	ASSERT_NE(secondUpload, nullptr);
	EXPECT_NE(secondUpload->stages.at(neurus::ShaderType::COMPUTE).spv, firstBytes);
	EXPECT_EQ(sink.LastGraphRequest()->graph->nodes[5]->data.shader, shader);

	// Publication owns the shared reference even after the project drops it.
	editor.GetRenderManager().GraphData().reset();
	editor.GetResourceManager().Clear();
	const std::weak_ptr<const neurus::ShaderData> lifetime = shader;
	shader.reset();
	EXPECT_FALSE(lifetime.expired());
	EXPECT_TRUE(first->nodes[5]->data.shader);
}

TEST(EditorRenderGraph, PublicationUploadsEachSelectedShaderOnce)
{
	RecordingSink sink;
	neurus::ResourceManager resources;
	neurus::RenderManager render(resources, &sink, std::make_shared<const neurus::BuiltInRenderData>());
	auto shader = resources.Load<neurus::ComputeShader>(
		"Shared", "res/shaders/compute/compose.comp.glsl");
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	// Use direct stored data to exercise publication independently of pass validation.
	render.GraphData() = std::make_shared<neurus::RenderGraphData>();
	neurus::CpuNode node;
	node.shader = shader;
	render.GraphData()->AddNode("first", node);
	render.GraphData()->AddNode("second", node);
	render.Publish();
	ASSERT_EQ(sink.posted.size(), 2u);
	EXPECT_TRUE(std::holds_alternative<neurus::UploadShaderRequest>(sink.posted[0]));
	EXPECT_TRUE(std::holds_alternative<neurus::CompileRenderGraphRequest>(sink.posted[1]));
}

TEST(EditorRenderGraph, ComputeCompileNotificationRepublishesReferencedShaderWithoutMeshWarm)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.Initialize();
	editor.NewScene();
	auto shader = editor.GetResourceManager().Load<neurus::ComputeShader>(
		"CustomCompose", "res/shaders/compute/compose.comp.glsl");
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	auto graph = neurus::CloneGraph(editor.GetRenderGraph());
	graph.nodes[5]->data.shader = shader;
	editor.SetRenderGraph(std::move(graph));
	sink.posted.clear();
	editor.GetEventQueue().Emit(neurus::ShaderCompiled{shader->GetObjectID()});
	ASSERT_EQ(sink.posted.size(), 2u);
	ASSERT_TRUE(std::holds_alternative<neurus::UploadShaderRequest>(sink.posted.front()));
	ASSERT_TRUE(std::holds_alternative<neurus::CompileRenderGraphRequest>(sink.posted.back()));
	ASSERT_TRUE(sink.LastGraphRequest()->graph->nodes[5]->data.shader);
}

TEST(EditorRenderGraph, GraphOnlyVertexShaderDoesNotWarmAMeshPipeline)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.Initialize();
	editor.NewScene();
	auto shader = editor.GetResourceManager().Load<neurus::RenderShader>(
		"CustomGeometry", "res/shaders/render/gbuffer.vert.glsl", "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	auto graph = neurus::CloneGraph(editor.GetRenderGraph());
	graph.nodes[0]->data.shader = shader;
	editor.SetRenderGraph(std::move(graph));
	sink.posted.clear();
	editor.GetEventQueue().Emit(neurus::ShaderCompiled{shader->GetObjectID()});
	ASSERT_EQ(sink.posted.size(), 2u);
	EXPECT_TRUE(std::holds_alternative<neurus::UploadShaderRequest>(sink.posted.front()));
	EXPECT_TRUE(std::holds_alternative<neurus::CompileRenderGraphRequest>(sink.posted.back()));
}

TEST(EditorRenderGraph, SharedMeshAndGraphShaderUploadsBeforeMappingAndPipelineWarm)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.Initialize();
	editor.NewScene();
	auto shader = editor.GetResourceManager().Load<neurus::RenderShader>(
		"SharedGeometry", "res/shaders/render/gbuffer.vert.glsl", "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	auto mesh = editor.GetResourceManager().Load<neurus::Mesh>();
	mesh->o_shader = shader;
	auto graph = neurus::CloneGraph(editor.GetRenderGraph());
	graph.nodes[0]->data.shader = shader;
	editor.SetRenderGraph(std::move(graph));
	sink.posted.clear();
	editor.GetEventQueue().Emit(neurus::ShaderCompiled{shader->GetObjectID()});
	ASSERT_EQ(sink.posted.size(), 3u);
	ASSERT_TRUE(std::holds_alternative<neurus::UploadShaderRequest>(sink.posted[0]));
	ASSERT_TRUE(std::holds_alternative<neurus::CompileRenderGraphRequest>(sink.posted[1]));
	const auto* ensure = std::get_if<neurus::EnsurePipelineRequest>(&sink.posted[2]);
	ASSERT_NE(ensure, nullptr);
	EXPECT_EQ(ensure->shaderId, shader->GetObjectID());
	EXPECT_EQ(ensure->version, shader->GetVersion());

	// A mesh-only shader follows the same upload path without remapping the graph.
	editor.GetRenderManager().GraphData().reset();
	sink.posted.clear();
	editor.GetEventQueue().Emit(neurus::ShaderCompiled{shader->GetObjectID()});
	ASSERT_EQ(sink.posted.size(), 2u);
	EXPECT_TRUE(std::holds_alternative<neurus::UploadShaderRequest>(sink.posted[0]));
	EXPECT_TRUE(std::holds_alternative<neurus::EnsurePipelineRequest>(sink.posted[1]));
}
