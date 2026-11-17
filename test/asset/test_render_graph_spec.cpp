/**
 * @file test_render_graph_spec.cpp
 * @brief Unit tests for the Vulkan-free render graph document.
 */

#include <gtest/gtest.h>

#include "asset/data/render_graph/RenderGraphSpec.hpp"

#include <cereal/archives/json.hpp>

#include <set>
#include <sstream>
#include <tuple>

namespace {

std::string Serialize(const neurus::RenderGraphSpec& spec)
{
	std::ostringstream stream;
	{
		cereal::JSONOutputArchive archive(stream);
		archive(cereal::make_nvp("graph", spec));
	}
	return stream.str();
}

neurus::RenderGraphSpec Deserialize(const std::string& text)
{
	std::istringstream stream(text);
	cereal::JSONInputArchive archive(stream);
	neurus::RenderGraphSpec spec;
	archive(cereal::make_nvp("graph", spec));
	return spec;
}

const neurus::GraphOutputTag& TagOf(const neurus::RenderGraphSpec& spec,
                                    const std::string& node,
                                    const std::string& port)
{
	const neurus::GraphNodeSpec* found = spec.FindNode(node);
	EXPECT_NE(found, nullptr) << "node " << node;
	static const neurus::GraphOutputTag empty;
	if (!found)
		return empty;

	const auto it = found->outputs.find(port);
	EXPECT_NE(it, found->outputs.end()) << "port " << node << "." << port;
	return it == found->outputs.end() ? empty : it->second;
}

} // anonymous namespace

TEST(RenderGraphSpec, DefaultHasSevenNodes)
{
	const neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();

	ASSERT_EQ(spec.nodes.size(), 7u);

	const std::vector<std::string> expectedIds = {
		"geometry", "shadow_depth", "shadow_intensity", "ssao",
		"lighting", "compose", "fxaa",
	};
	const std::vector<std::string> expectedPasses = {
		"GeometryPass", "ShadowDepthPass", "ShadowIntensityPass", "SSAOPass",
		"LightingPass", "ComposePass", "FXAAPass",
	};

	for (size_t i = 0; i < expectedIds.size(); ++i)
	{
		EXPECT_EQ(spec.nodes[i].id, expectedIds[i]);
		EXPECT_EQ(spec.nodes[i].pass, expectedPasses[i]);
	}
}

TEST(RenderGraphSpec, DefaultGeometryTags)
{
	const neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();

	EXPECT_EQ(TagOf(spec, "geometry", "gPosition").format, neurus::GraphFormat::Rgba16f);
	EXPECT_EQ(TagOf(spec, "geometry", "gPosition").usage, neurus::GraphUsage::ColorSampled);
	EXPECT_EQ(TagOf(spec, "geometry", "gNormal").format, neurus::GraphFormat::Rgba16f);
	EXPECT_EQ(TagOf(spec, "geometry", "gAlbedo").format, neurus::GraphFormat::Rgba8Srgb);
	EXPECT_EQ(TagOf(spec, "geometry", "gMetallicRoughness").format, neurus::GraphFormat::Rgba8Unorm);
	EXPECT_EQ(TagOf(spec, "geometry", "gObjectID").format, neurus::GraphFormat::R32Uint);
	EXPECT_EQ(TagOf(spec, "geometry", "depth").format, neurus::GraphFormat::D32Sfloat);
	EXPECT_EQ(TagOf(spec, "geometry", "depth").usage, neurus::GraphUsage::DepthSampled);
}

TEST(RenderGraphSpec, DefaultShadowTags)
{
	const neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();

	EXPECT_EQ(TagOf(spec, "shadow_depth", "shadow_depth").usage, neurus::GraphUsage::Logical);

	const neurus::GraphOutputTag& intensity = TagOf(spec, "shadow_intensity", "outputShadow");
	EXPECT_EQ(intensity.format, neurus::GraphFormat::R8Unorm);
	EXPECT_EQ(intensity.usage, neurus::GraphUsage::StorageSampled);
	EXPECT_EQ(intensity.extent, neurus::GraphExtent::Render);
	EXPECT_EQ(intensity.layers, neurus::GraphLayers::Lights);
}

TEST(RenderGraphSpec, DefaultEdgesAreUnique)
{
	const neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();

	ASSERT_EQ(spec.edges.size(), 14u);

	std::set<std::tuple<std::string, std::string, std::string, std::string>> seen;
	for (const neurus::GraphEdgeSpec& edge : spec.edges)
		seen.emplace(edge.from, edge.fromPort, edge.to, edge.toPort);

	EXPECT_EQ(seen.size(), spec.edges.size());
	EXPECT_TRUE(seen.contains({"geometry", "gPosition", "ssao", "gPosition"}));
	EXPECT_TRUE(seen.contains({"compose", "outputComposed", "fxaa", "gInput"}));
}

TEST(RenderGraphSpec, RoundTrip)
{
	const neurus::RenderGraphSpec original = neurus::MakeDefaultRenderGraphSpec();
	const neurus::RenderGraphSpec restored = Deserialize(Serialize(original));

	EXPECT_EQ(restored, original);
	EXPECT_EQ(neurus::RenderGraphSpecHash(restored), neurus::RenderGraphSpecHash(original));
}

TEST(RenderGraphSpec, PruneRemovesFxaaAndItsEdges)
{
	const neurus::RenderGraphSpec pruned =
		neurus::PruneConditionalNodes(neurus::MakeDefaultRenderGraphSpec(), false);

	ASSERT_EQ(pruned.nodes.size(), 6u);
	ASSERT_EQ(pruned.edges.size(), 13u);

	for (const neurus::GraphNodeSpec& node : pruned.nodes)
		EXPECT_NE(node.pass, "FXAAPass");

	EXPECT_EQ(pruned.FindNode("fxaa"), nullptr);
	for (const neurus::GraphEdgeSpec& edge : pruned.edges)
	{
		EXPECT_NE(edge.from, "fxaa");
		EXPECT_NE(edge.to, "fxaa");
	}
}

TEST(RenderGraphSpec, PruneKeepsFxaaWhenEnabled)
{
	const neurus::RenderGraphSpec original = neurus::MakeDefaultRenderGraphSpec();
	const neurus::RenderGraphSpec kept = neurus::PruneConditionalNodes(original, true);

	EXPECT_EQ(kept, original);
}

TEST(RenderGraphSpec, HashChangesWithTopology)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	const std::uint64_t original = neurus::RenderGraphSpecHash(spec);

	neurus::RenderGraphSpec removed = spec;
	removed.nodes.pop_back();
	EXPECT_NE(neurus::RenderGraphSpecHash(removed), original);

	neurus::RenderGraphSpec retagged = spec;
	retagged.FindNode("geometry")->outputs["gPosition"].format = neurus::GraphFormat::Rgba8Unorm;
	EXPECT_NE(neurus::RenderGraphSpecHash(retagged), original);

	neurus::RenderGraphSpec rewired = spec;
	rewired.edges.pop_back();
	EXPECT_NE(neurus::RenderGraphSpecHash(rewired), original);
}

TEST(RenderGraphSpec, HashStableAcrossCopies)
{
	const neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	const neurus::RenderGraphSpec copy = spec;

	EXPECT_EQ(neurus::RenderGraphSpecHash(copy), neurus::RenderGraphSpecHash(spec));
}
