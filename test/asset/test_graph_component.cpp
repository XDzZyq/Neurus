/**
 * @file test_graph_component.cpp
 * @brief Project persistence for the render graph document.
 */

#include <gtest/gtest.h>

#include "shared/TestPassPorts.hpp"

#include "asset/components/GraphComponent.hpp"
#include "asset/data/RenderGraphData.hpp"

#include <cereal/archives/json.hpp>

#include <optional>
#include <sstream>
#include <string>

namespace {

std::string Save(const neurus::RenderGraphData& graph)
{
	std::ostringstream stream;
	{
		cereal::JSONOutputArchive archive(stream);
		std::optional<neurus::RenderGraphRecord> pending;
		neurus::project::GraphComponent component(pending, graph);
		component.Save(archive);
	}
	return stream.str();
}

std::optional<neurus::RenderGraphRecord> Load(const std::string& text)
{
	std::optional<neurus::RenderGraphRecord> pending;
	std::istringstream stream(text);
	cereal::JSONInputArchive archive(stream);
	const neurus::RenderGraphData empty;
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

	const neurus::RenderGraphRecord expected = graph.ToRecord();
	const std::optional<neurus::RenderGraphRecord> restored = Load(Save(graph));

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
