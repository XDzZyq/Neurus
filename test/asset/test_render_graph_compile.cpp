/**
 * @file test_render_graph_compile.cpp
 * @brief Validation and topological planning of RenderGraphSpec.
 */

#include <gtest/gtest.h>

#include "shared/TestPassPorts.hpp"

#include "asset/graph/PassPorts.hpp"
#include "asset/graph/RenderGraphCompile.hpp"

#include <string>

namespace {

std::string DiagnosticsText(const neurus::CompileResult& result)
{
	std::string text;
	for (const neurus::GraphDiagnostic& diagnostic : result.diagnostics)
	{
		text += diagnostic.node + "." + diagnostic.port + ": "
		      + diagnostic.message + "\n";
	}
	return text;
}

bool HasDiagnostic(const neurus::CompileResult& result,
                   const std::string& node,
                   const std::string& needle)
{
	for (const neurus::GraphDiagnostic& diagnostic : result.diagnostics)
	{
		if (!node.empty() && diagnostic.node != node)
			continue;
		if (diagnostic.port.find(needle) != std::string::npos
		    || diagnostic.message.find(needle) != std::string::npos)
			return true;
	}
	return false;
}

size_t PositionOf(const neurus::CompileResult& result, const std::string& id)
{
	for (size_t i = 0; i < result.order.size(); ++i)
	{
		if (result.spec.nodes[result.order[i]].id == id)
			return i;
	}
	return result.order.size();
}

} // anonymous namespace

TEST(RenderGraphCompile, DefaultSpecCompiles)
{
	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(
		neurus::MakeDefaultRenderGraphSpec(), source, true);

	ASSERT_TRUE(result.ok) << DiagnosticsText(result);
	ASSERT_EQ(result.spec.nodes.size(), 7u);
	ASSERT_EQ(result.order.size(), 7u);

	EXPECT_LT(PositionOf(result, "geometry"), PositionOf(result, "ssao"));
	EXPECT_LT(PositionOf(result, "ssao"), PositionOf(result, "lighting"));
	EXPECT_LT(PositionOf(result, "lighting"), PositionOf(result, "compose"));
	EXPECT_LT(PositionOf(result, "compose"), PositionOf(result, "fxaa"));
	EXPECT_LT(PositionOf(result, "shadow_depth"), PositionOf(result, "shadow_intensity"));
	EXPECT_LT(PositionOf(result, "shadow_intensity"), PositionOf(result, "lighting"));
}

TEST(RenderGraphCompile, DefaultWithoutFxaaCompiles)
{
	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(
		neurus::MakeDefaultRenderGraphSpec(), source, false);

	ASSERT_TRUE(result.ok) << DiagnosticsText(result);
	ASSERT_EQ(result.spec.nodes.size(), 6u);
	ASSERT_EQ(result.order.size(), 6u);
	EXPECT_EQ(result.spec.nodes[result.order.back()].pass, "ComposePass");
}

TEST(RenderGraphCompile, UnknownPassIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.nodes[0].pass = "NopePass";

	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "geometry", "NopePass"));
}

TEST(RenderGraphCompile, UnknownPortIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.edges[0].toPort = "bogus";

	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "ssao", "bogus"));
}

TEST(RenderGraphCompile, WrongDirectionIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.edges[0].from = "ssao";
	spec.edges[0].fromPort = "gPosition";

	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "ssao", "input, not an output"));
}

TEST(RenderGraphCompile, DoubleProducerIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.edges.push_back({"geometry", "gNormal", "lighting", "gPosition"});

	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "lighting", "more than one producer"));
}

TEST(RenderGraphCompile, MissingTagIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.FindNode("geometry")->outputs.erase("gPosition");

	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "geometry", "no tag"));
}

TEST(RenderGraphCompile, TagForUnknownPortIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.FindNode("geometry")->outputs["nope"] =
		{neurus::GraphFormat::Rgba16f, neurus::GraphUsage::ColorSampled};

	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "geometry", "unknown output port"));
}

TEST(RenderGraphCompile, UnconnectedInputIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	std::erase_if(spec.edges, [](const neurus::GraphEdgeSpec& edge)
	{
		return edge.from == "ssao" && edge.fromPort == "outputSSAO"
		    && edge.to == "lighting" && edge.toPort == "U_AO";
	});

	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "lighting", "not connected"));
}

TEST(RenderGraphCompile, ExternalInputIsAccepted)
{
	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(
		neurus::MakeDefaultRenderGraphSpec(), source, true);

	ASSERT_TRUE(result.ok) << DiagnosticsText(result);
	EXPECT_FALSE(HasDiagnostic(result, "lighting", "U_Irradiance"));
	EXPECT_FALSE(HasDiagnostic(result, "lighting", "U_Prefiltered"));
}

TEST(RenderGraphCompile, CycleIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	std::erase_if(spec.edges, [](const neurus::GraphEdgeSpec& edge)
	{
		return edge.from == "geometry" && edge.fromPort == "gPosition"
		    && edge.to == "ssao" && edge.toPort == "gPosition";
	});
	spec.edges.push_back({"compose", "outputComposed", "ssao", "gPosition"});

	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, false);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "", "cycle detected"));
	EXPECT_TRUE(HasDiagnostic(result, "", "ssao"));
	EXPECT_TRUE(HasDiagnostic(result, "", "compose"));
}

TEST(RenderGraphCompile, OrderIsDeterministic)
{
	const neurus::test::FakePortSource source = neurus::test::MakeRealPortSource();
	const neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();

	const neurus::CompileResult first = neurus::CompileRenderGraphSpec(spec, source, true);
	const neurus::CompileResult second = neurus::CompileRenderGraphSpec(spec, source, true);

	ASSERT_TRUE(first.ok) << DiagnosticsText(first);
	ASSERT_TRUE(second.ok) << DiagnosticsText(second);
	EXPECT_EQ(first.order, second.order);
}
