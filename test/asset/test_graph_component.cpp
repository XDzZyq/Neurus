/**
 * @file test_graph_component.cpp
 * @brief Project persistence for the render graph document.
 */

#include <gtest/gtest.h>

#include "asset/components/GraphComponent.hpp"
#include "asset/data/render_graph/RenderGraphSpec.hpp"

#include <cereal/archives/json.hpp>

#include <sstream>
#include <string>

namespace {

std::string Save(neurus::RenderGraphSpec& spec)
{
	std::ostringstream stream;
	{
		cereal::JSONOutputArchive archive(stream);
		neurus::project::GraphComponent component(spec);
		component.Save(archive);
	}
	return stream.str();
}

void Load(neurus::RenderGraphSpec& spec, const std::string& text)
{
	std::istringstream stream(text);
	cereal::JSONInputArchive archive(stream);
	neurus::project::GraphComponent component(spec);
	component.Load(archive);
}

} // anonymous namespace

TEST(GraphComponent, RoundTrip)
{
	neurus::RenderGraphSpec original = neurus::MakeDefaultRenderGraphSpec();
	original.FindNode("geometry")->outputs["gPosition"].format = "rgba32f";
	original.edges.pop_back();

	neurus::RenderGraphSpec restored = neurus::MakeDefaultRenderGraphSpec();
	Load(restored, Save(original));

	EXPECT_EQ(restored, original);
}

TEST(GraphComponent, MissingNodeFallsBack)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.nodes.pop_back();

	Load(spec, "{}");

	EXPECT_EQ(spec, neurus::MakeDefaultRenderGraphSpec());
}

TEST(GraphComponent, CorruptNodeFallsBack)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.nodes.pop_back();

	Load(spec, "{\"proj_render_graph\": 42}");

	EXPECT_EQ(spec, neurus::MakeDefaultRenderGraphSpec());
}
