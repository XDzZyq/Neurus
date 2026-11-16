/**
 * @file test_editor_render_graph.cpp
 * @brief The Editor owns the document and publishes it as a compile request.
 */

#include <gtest/gtest.h>

#include "editor/Editor.hpp"
#include "render/requests/RenderRequestSink.hpp"
#include "render/requests/RenderRequests.hpp"

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

TEST(EditorRenderGraph, FinishLoadPostsTheDocument)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);

	editor.GetRenderGraphSpec().nodes.pop_back();
	editor.FinishLoad();

	const neurus::CompileRenderGraphRequest* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_EQ(request->spec, editor.GetRenderGraphSpec());
}

TEST(EditorRenderGraph, NewSceneResetsAndPostsTheDefault)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);

	editor.GetRenderGraphSpec().nodes.pop_back();
	editor.NewScene();

	EXPECT_EQ(editor.GetRenderGraphSpec(), neurus::MakeDefaultRenderGraphSpec());

	const neurus::CompileRenderGraphRequest* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_EQ(request->spec, neurus::MakeDefaultRenderGraphSpec());
}

TEST(EditorRenderGraph, SetPostsAndMarksDirty)
{
	RecordingSink sink;
	neurus::Editor editor(&sink);
	editor.ClearDirty();

	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.nodes.pop_back();
	editor.SetRenderGraphSpec(spec);

	EXPECT_TRUE(editor.IsDirty());
	EXPECT_EQ(editor.GetRenderGraphSpec(), spec);

	const neurus::CompileRenderGraphRequest* request = sink.LastGraphRequest();
	ASSERT_NE(request, nullptr);
	EXPECT_EQ(request->spec, spec);
}
