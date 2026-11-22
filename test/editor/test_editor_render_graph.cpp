/**
 * @file test_editor_render_graph.cpp
 * @brief Nullable project render state stays separate from cached CPU defaults.
 */

#include <gtest/gtest.h>

#include "editor/Editor.hpp"
#include "editor/project/HistoryComponent.hpp"
#include "core/ResourceManager.hpp"
#include "editor/RenderManager.hpp"
#include "asset/components/GraphComponent.hpp"
#include "asset/components/ConfigComponent.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "shader/ComputeShader.hpp"
#include "shader/RenderShader.hpp"
#include "scene/Mesh.hpp"
#include "editor/events/ShaderEvents.hpp"
#include "editor/events/GraphEvents.hpp"
#include "editor/events/ScriptEvents.hpp"
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
	record.outputs.reset(); // Historic inline format inferred the final pass.
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

	auto graph = neurus::CloneGraph(editor.GetRenderGraph());
	graph.RemoveNode(graph.nodes.back().get());
	graph.sceneOutputs["color"] = {"compose", "outputComposed"};
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
	auto graph = neurus::CloneGraph(editor.GetRenderGraph());
	graph.RemoveNode(graph.nodes.back().get());
	graph.sceneOutputs["color"] = {"compose", "outputComposed"};
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
	EXPECT_EQ(std::get<neurus::project::RenderGraphRecord>(*render.PendingRecord()), expected);
	EXPECT_TRUE(sink.posted.empty());
}

TEST(EditorRenderGraph, ResetGraphPreservesConfig)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();
	auto graph = neurus::CloneGraph(editor.GetRenderGraph());
	graph.RemoveNode(graph.nodes.back().get());
	graph.sceneOutputs["color"] = {"compose", "outputComposed"};
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
		neurus::project::ResourceComponent resources(editor.GetResourceManager());
		resources.Save(archive);
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
		neurus::project::ResourceComponent resources(editor.GetResourceManager());
		resources.LoadJson(text);
		auto& render = editor.GetRenderManager();
		neurus::project::GraphComponent graph(render.PendingRecord(), render.GraphData());
		graph.LoadJson(text);
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
	editor.SetRenderGraph(neurus::RenderGraphTopology{});
	ASSERT_TRUE(editor.GetRenderManager().GraphData());
	const int graphUid = editor.GetRenderManager().GraphData()->GetObjectID();
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
	EXPECT_EQ(editor.GetRenderManager().GraphData()->GetObjectID(), graphUid);
	EXPECT_FALSE(editor.GetRenderConfigData());
}

TEST(EditorRenderGraph, EditsAndPublicationPreserveOnePooledGraphIdentity)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();
	auto& render = editor.GetRenderManager();
	const auto resource = render.CreateGraph();
	const int uid = resource->GetObjectID();
	EXPECT_EQ(resource, editor.GetResourceManager().Get<neurus::RenderGraphData>(uid));
	EXPECT_EQ(resource, render.CreateGraph());

	auto topology = neurus::CloneGraph(render.Graph());
	topology.RemoveNode(topology.nodes.back().get());
	topology.sceneOutputs["color"] = {"compose", "outputComposed"};
	const auto allocated = neurus::UID::GetTotalAllocated();
	editor.SetRenderGraph(std::move(topology));
	EXPECT_EQ(resource, render.GraphData());
	EXPECT_EQ(sink.LastGraphRequest()->graph->nodes.size(), 6u);
	EXPECT_EQ(neurus::UID::GetTotalAllocated(), allocated);
}

TEST(EditorRenderGraph, MissingActiveUidPreservesTheRestOfThePool)
{
	neurus::Editor editor(nullptr);
	editor.NewScene();
	auto resource = editor.GetResourceManager().Load<neurus::Mesh>();
	auto& render = editor.GetRenderManager();
	render.PendingRecord() = resource->GetObjectID();
	render.FinishGraphLoad();
	EXPECT_FALSE(render.GraphData());
	EXPECT_FALSE(render.Problems().empty());
	EXPECT_EQ(editor.GetResourceManager().Get<neurus::Mesh>(resource->GetObjectID()), resource);
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
	editor.GetRenderManager().ResetGraph();
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
	const auto graph = render.CreateGraph();
	graph->nodes[5]->data.shader = shader;
	graph->nodes[6]->data.shader = shader;
	sink.posted.clear();
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
	editor.GetRenderManager().ResetGraph();
	sink.posted.clear();
	editor.GetEventQueue().Emit(neurus::ShaderCompiled{shader->GetObjectID()});
	ASSERT_EQ(sink.posted.size(), 2u);
	EXPECT_TRUE(std::holds_alternative<neurus::UploadShaderRequest>(sink.posted[0]));
	EXPECT_TRUE(std::holds_alternative<neurus::EnsurePipelineRequest>(sink.posted[1]));
}


TEST(EditorRenderGraph, FailedScriptDiscardsGraphCreationAndPooledShader)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.Initialize();
	editor.NewScene();
	auto& bus = editor.GetEventQueue();
	const auto before = editor.GetResourceManager().Size();
	sink.posted.clear();
	bus.Emit(neurus::ScriptRunStarted{"Create and fail"});
	bus.Emit(neurus::RenderGraphCreateRequested{});
	int shader = -1;
	bus.Emit(neurus::RenderGraphCreateShaderRequested{"compose", &shader});
	ASSERT_TRUE(editor.GetResourceManager().Get<neurus::ShaderData>(shader));
	bus.Emit(neurus::RenderGraphDisconnectRequested{{"", "color"}});
	std::string diagnostic;
	bus.Emit(neurus::ScriptRunFinished{true, &diagnostic});
	EXPECT_FALSE(diagnostic.empty());
	EXPECT_FALSE(editor.GetRenderManager().GraphData());
	EXPECT_EQ(before, editor.GetResourceManager().Size());
	EXPECT_TRUE(editor.GetHistory().undo.empty());
	EXPECT_TRUE(sink.posted.empty());
	EXPECT_FALSE(editor.IsDirty());
}

TEST(EditorRenderGraph, BypassAndRenamePublishOnceAndUndoTogether)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.Initialize();
	editor.NewScene();
	auto& bus = editor.GetEventQueue();
	bus.Emit(neurus::RenderGraphCreateRequested{});
	sink.posted.clear();
	bus.Emit(neurus::ScriptRunStarted{"Edit output"});
	bus.Emit(neurus::RenderGraphDisconnectRequested{{"", "color"}});
	bus.Emit(neurus::RenderGraphRenameRequested{"compose", "finish"});
	bus.Emit(neurus::RenderGraphConnectRequested{{"finish", "outputComposed"}, {"", "color"}});
	EXPECT_TRUE(sink.posted.empty());
	std::string error;
	bus.Emit(neurus::ScriptRunFinished{true, &error});
	ASSERT_TRUE(error.empty()) << error;
	EXPECT_EQ(editor.GetRenderGraph().nodes.size(), 7u);
	const auto& selected = editor.GetRenderGraph().sceneOutputs.at("color");
	EXPECT_EQ(selected.node, "finish");
	auto count = std::count_if(sink.posted.begin(), sink.posted.end(), [](const auto& request) {
		return std::holds_alternative<neurus::CompileRenderGraphRequest>(request);
	});
	EXPECT_EQ(count, 1);
	editor.GetOperations().Undo();
	EXPECT_EQ(editor.GetRenderGraph().sceneOutputs.at("color").node, "fxaa");
	EXPECT_EQ(editor.GetRenderGraph().nodes[5]->name, "compose");
	editor.GetOperations().Redo();
	EXPECT_EQ(editor.GetRenderGraph().sceneOutputs.at("color").node, "finish");
}

TEST(EditorRenderGraph, IndependentPassShadersCanBeSharedAndCleared)
{
	neurus::Editor editor(nullptr);
	editor.Initialize();
	editor.NewScene();
	auto& bus = editor.GetEventQueue();
	bus.Emit(neurus::RenderGraphCreateRequested{});
	bus.Emit(neurus::ScriptRunStarted{"Create two shaders"});
	std::string name;
	bus.Emit(neurus::RenderGraphAddRequested{"ComposePass", "other", &name});
	bus.Emit(neurus::RenderGraphConnectRequested{{"lighting", "outputImage"}, {"other", "gHDRColor"}});
	int first = -1, second = -1, repeated = -1;
	bus.Emit(neurus::RenderGraphCreateShaderRequested{"compose", &first});
	bus.Emit(neurus::RenderGraphCreateShaderRequested{"other", &second});
	bus.Emit(neurus::RenderGraphCreateShaderRequested{"other", &repeated});
	EXPECT_NE(first, second);
	EXPECT_EQ(second, repeated);
	std::string diagnostic;
	bus.Emit(neurus::ScriptRunFinished{true, &diagnostic});
	ASSERT_TRUE(diagnostic.empty()) << diagnostic;

	bus.Emit(neurus::RenderGraphShaderRequested{"other", first});
	EXPECT_EQ(editor.GetRenderGraph().nodes.back()->data.shader, editor.GetRenderGraph().nodes[5]->data.shader);
	bus.Emit(neurus::RenderGraphShaderRequested{"other", {}});
	EXPECT_FALSE(editor.GetRenderGraph().nodes.back()->data.shader);
	editor.GetOperations().Undo();
	EXPECT_EQ(editor.GetRenderGraph().nodes.back()->data.shader, editor.GetRenderGraph().nodes[5]->data.shader);
}

TEST(EditorRenderGraph, StageWritesCompileTogetherAndAnInvalidBatchPreservesLastGoodShader)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.Initialize();
	editor.NewScene();
	auto& bus = editor.GetEventQueue();
	bus.Emit(neurus::RenderGraphCreateRequested{});
	int uid = -1;
	bus.Emit(neurus::RenderGraphCreateShaderRequested{"compose", &uid});
	const auto shader = editor.GetResourceManager().Get<neurus::ShaderData>(uid);
	ASSERT_TRUE(shader);
	const auto& unit = shader->GetStage(neurus::ShaderType::COMPUTE);
	const std::string original = unit.code;
	constexpr int stage = static_cast<int>(neurus::ShaderType::COMPUTE);
	const int version = shader->GetVersion();
	const auto initialHistory = editor.GetHistory().undo.size();
	sink.posted.clear();
	editor.ClearDirty();

	// Intermediate draft source may be incomplete. Reverting before commit is a no-op.
	bus.Emit(neurus::ScriptRunStarted{"Draft"});
	bus.Emit(neurus::ShaderUnitCodeSet{uid, stage, "broken intermediate edit"});
	bus.Emit(neurus::ShaderUnitCodeSet{uid, stage, original});
	std::string error;
	bus.Emit(neurus::ScriptRunFinished{true, &error});
	ASSERT_TRUE(error.empty()) << error;
	EXPECT_EQ(shader->GetVersion(), version);
	EXPECT_EQ(editor.GetHistory().undo.size(), initialHistory);
	EXPECT_TRUE(sink.posted.empty());

	bus.Emit(neurus::ScriptRunStarted{"Invalid"});
	bus.Emit(neurus::ShaderUnitCodeSet{uid, stage, "#version 450\ninvalid GLSL"});
	bus.Emit(neurus::ScriptRunFinished{true, &error});
	EXPECT_NE(error.find("compose"), std::string::npos);
	EXPECT_EQ(unit.code, original);
	EXPECT_EQ(shader->GetVersion(), version);
	EXPECT_EQ(editor.GetHistory().undo.size(), initialHistory);
	EXPECT_TRUE(sink.posted.empty());
	EXPECT_FALSE(editor.IsDirty());
}

TEST(EditorRenderGraph, SavedRedoRecreatesGraphAndEditedShaderWithOriginalIdentities)
{
	neurus::Editor editor(nullptr);
	editor.Initialize();
	editor.NewScene();
	auto& bus = editor.GetEventQueue();
	bus.Emit(neurus::ScriptRunStarted{"Customize compose"});
	bus.Emit(neurus::RenderGraphCreateRequested{});
	const int graphUid = editor.GetRenderManager().GraphData()->GetObjectID();
	int shaderUid = -1;
	bus.Emit(neurus::RenderGraphCreateShaderRequested{"compose", &shaderUid});
	const auto shader = editor.GetResourceManager().Get<neurus::ShaderData>(shaderUid);
	ASSERT_TRUE(shader);
	const auto stage = neurus::ShaderType::COMPUTE;
	const auto code = shader->GetStage(stage).code + "\n// project-owned edit\n";
	bus.Emit(neurus::ShaderUnitCodeSet{shaderUid, static_cast<int>(stage), code});
	std::string error;
	bus.Emit(neurus::ScriptRunFinished{true, &error});
	ASSERT_TRUE(error.empty()) << error;
	editor.GetOperations().Undo();
	ASSERT_FALSE(editor.GetRenderManager().GraphData());
	ASSERT_FALSE(editor.GetResourceManager().Get<neurus::ShaderData>(shaderUid));
	ASSERT_EQ(editor.GetHistory().redo.size(), 1u);

	std::stringstream savedHistory;
	{
		cereal::JSONOutputArchive archive(savedHistory);
		neurus::project::HistoryComponent(editor.GetOperations()).Save(archive);
	}
	neurus::Editor reopened(nullptr);
	reopened.Initialize();
	LoadOptionalRenderState(reopened, SaveOptionalRenderState(editor));
	{
		cereal::JSONInputArchive archive(savedHistory);
		neurus::project::HistoryComponent(reopened.GetOperations()).Load(archive);
	}
	reopened.GetOperations().Redo();
	const auto& graph = reopened.GetRenderManager().GraphData();
	ASSERT_TRUE(graph);
	EXPECT_EQ(graph->GetObjectID(), graphUid);
	const auto restoredShader = reopened.GetResourceManager().Get<neurus::ShaderData>(shaderUid);
	ASSERT_TRUE(restoredShader);
	EXPECT_EQ(graph->nodes[5]->data.shader, restoredShader);
	EXPECT_EQ(restoredShader->GetStage(stage).code, code);
	EXPECT_TRUE(restoredShader->IsValid());
}

TEST(EditorRenderGraph, LoadingIncompatiblePooledShaderUsesOnlyThatPassDefault)
{
	neurus::Editor editor(nullptr);
	editor.NewScene();
	auto& render = editor.GetRenderManager();
	const auto shader = editor.GetResourceManager().Load<neurus::ComputeShader>(
		"Incompatible", "res/shaders/compute/fxaa.comp.glsl");
	ASSERT_TRUE(shader->ParseAndGenerate());
	ASSERT_TRUE(shader->CompileToSpv());
	auto saved = neurus::project::ToRecord(render.Graph());
	saved.nodes[5].shaderUid = shader->GetObjectID();
	render.PendingRecord() = saved;
	render.FinishGraphLoad();
	ASSERT_TRUE(render.GraphData());
	EXPECT_EQ(render.Graph().nodes.size(), 7u);
	EXPECT_FALSE(render.Graph().nodes[5]->data.shader);
	EXPECT_TRUE(editor.GetResourceManager().Get<neurus::ShaderData>(shader->GetObjectID()));
	EXPECT_FALSE(render.Problems().empty());
}
