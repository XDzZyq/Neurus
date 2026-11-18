/**
 * @file test_editor_render_graph.cpp
 * @brief The Editor owns the document, seeds its shaders and publishes it.
 */

#include <gtest/gtest.h>

#include "editor/Editor.hpp"
#include "asset/data/ShaderData.hpp"
#include "render/requests/RenderRequestSink.hpp"
#include "render/requests/RenderRequests.hpp"

#include <algorithm>
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

} // anonymous namespace

TEST(EditorRenderGraph, NewSceneSeedsShadersAndPostsTheDocument)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.NewScene();

	const neurus::RenderGraphData& graph = editor.GetRenderGraph();
	ASSERT_EQ(graph.nodes.size(), 7u);

	for (const auto& node : graph.nodes)
	{
		EXPECT_NE(node->data.shaderUid, 0) << node->name;
		EXPECT_NE(editor.GetResourceManager().Get<neurus::ShaderData>(
			node->data.shaderUid), nullptr) << node->name;
	}

	EXPECT_TRUE(editor.GetGraphProblems().empty());
	EXPECT_FALSE(editor.IsDirty());

	const neurus::CompileRenderGraphRequest* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_EQ(request->graph->ToRecord(), graph.ToRecord());

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

	neurus::RenderGraphRecord record = editor.GetRenderGraph().ToRecord();
	record.nodes.pop_back();
	record.edges.erase(
		std::remove_if(record.edges.begin(), record.edges.end(),
			[](const neurus::RenderGraphEdgeRecord& edge)
			{
				return edge.from == "fxaa" || edge.to == "fxaa";
			}),
		record.edges.end());
	editor.GetPendingRenderGraph() = record;

	editor.FinishLoad();

	EXPECT_EQ(editor.GetRenderGraph().nodes.size(), 6u);
	EXPECT_EQ(editor.GetRenderGraph().nodes[5]->name, "compose");
	EXPECT_FALSE(editor.IsDirty());

	const neurus::CompileRenderGraphRequest* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_EQ(request->graph->ToRecord(), editor.GetRenderGraph().ToRecord());
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
	EXPECT_EQ(request->graph->ToRecord(), editor.GetRenderGraph().ToRecord());
}
