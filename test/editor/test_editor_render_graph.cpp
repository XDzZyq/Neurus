/**
 * @file test_editor_render_graph.cpp
 * @brief The Editor owns the document, seeds its shaders and publishes it.
 */

#include <gtest/gtest.h>

#include "editor/Editor.hpp"
#include "editor/RenderManager.hpp"
#include "asset/components/GraphComponent.hpp"
#include "asset/components/ConfigComponent.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"
#include "render_api/RenderRequestSink.hpp"
#include "render_api/RenderRequests.hpp"

#include <algorithm>
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

TEST(EditorRenderGraph, NewSceneSeedsShadersAndPostsTheDocument)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.GetRenderConfig() = NonDefaultConfig();
	editor.NewScene();
	EXPECT_EQ(editor.GetRenderConfig(), neurus::RenderConfig{});

	const neurus::RenderGraphData& graph = editor.GetRenderGraph();
	ASSERT_EQ(graph.nodes.size(), 7u);

	for (const auto& node : graph.nodes)
	{
		EXPECT_NE(node->data.shaderUid, 0) << node->name;
		EXPECT_NE(editor.GetResourceManager().Get<neurus::ShaderData>(
			node->data.shaderUid), nullptr) << node->name;
	}

	EXPECT_TRUE(editor.GetRenderManager().Problems().empty());
	EXPECT_FALSE(editor.IsDirty());

	const neurus::CompileRenderGraphRequest* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_EQ(neurus::project::ToRecord(*request->graph), neurus::project::ToRecord(graph));

	for (const auto& node : graph.nodes)
	{
		const bool present = std::any_of(
			request->shaders.begin(), request->shaders.end(),
			[&node](const neurus::ShaderSnapshot& snapshot)
			{
				return snapshot.shaderUid == node->data.shaderUid;
			});
		EXPECT_TRUE(present) << node->name;
	}
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
	editor.GetRenderConfig() = expected;

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
	neurus::RenderConfig expected = NonDefaultConfig();
	std::stringstream serialized;
	{
		cereal::JSONOutputArchive archive(serialized);
		neurus::project::ConfigComponent config(expected);
		config.Save(archive);
	}

	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();
	editor.GetRenderConfig() = expected;
	editor.BeginLoad();
	ASSERT_EQ(editor.GetRenderConfig(), neurus::RenderConfig{});
	{
		cereal::JSONInputArchive archive(serialized);
		neurus::project::ConfigComponent config(editor.GetRenderConfig());
		config.Load(archive);
		neurus::RenderManager& render = editor.GetRenderManager();
		neurus::project::GraphComponent graph(render.PendingRecord(), render.Graph());
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
	render.Config() = NonDefaultConfig();
	const auto expected = neurus::project::ToRecord(render.Graph());
	render.PendingRecord() = expected;
	sink.posted.clear();

	render.ResetConfig();

	EXPECT_EQ(render.Config(), neurus::RenderConfig{});
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
	editor.GetRenderConfig() = expected;
	sink.posted.clear();

	editor.GetRenderManager().ResetGraph();

	EXPECT_EQ(editor.GetRenderConfig(), expected);
	EXPECT_EQ(editor.BuildSnapshot().Settings(), expected);
	EXPECT_EQ(editor.GetRenderGraph().nodes.size(), 7u);
	const neurus::CompileRenderGraphRequest* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_EQ(neurus::project::ToRecord(*request->graph),
	          neurus::project::ToRecord(editor.GetRenderGraph()));
}
