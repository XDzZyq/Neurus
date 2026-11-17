/**
 * @file test_render_graph_compile.cpp
 * @brief Validation and topological planning of RenderGraphSpec.
 */

#include <gtest/gtest.h>

#include "asset/graph/PassPorts.hpp"
#include "asset/graph/RenderGraphCompile.hpp"
#include "render/shaders/ComputeShader.hpp"
#include "render/shaders/RenderShader.hpp"
#include "render/shaders/ShaderLibrary.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

class FakePortSource : public neurus::IPortSource
{
public:
	void Set(std::string kind, neurus::PassPorts ports)
	{
		m_ports[std::move(kind)] = std::move(ports);
	}

	void SetExternal(std::string kind, std::vector<std::string> names)
	{
		m_externals[std::move(kind)] = std::move(names);
	}

	const neurus::PassPorts* FindPassPorts(const std::string_view kind) const override
	{
		const auto it = m_ports.find(std::string(kind));
		return it == m_ports.end() ? nullptr : &it->second;
	}

	std::vector<std::string> ExternalInputs(const std::string_view kind) const override
	{
		const auto it = m_externals.find(std::string(kind));
		return it == m_externals.end() ? std::vector<std::string>{} : it->second;
	}

private:
	std::unordered_map<std::string, neurus::PassPorts>            m_ports;
	std::unordered_map<std::string, std::vector<std::string>>     m_externals;
};

FakePortSource MakeRealPortSource()
{
	FakePortSource source;

	auto geometry = neurus::ShaderLibrary::LoadRenderShader(
		"GeometryPass",
		"res/shaders/render/gbuffer.vert.glsl",
		"res/shaders/render/gbuffer.frag.glsl");
	EXPECT_NE(geometry, nullptr);

	const std::array<const neurus::ShaderData*, 1> geometryShaders = {geometry.get()};
	const std::array<neurus::ShaderPort, 1> geometryDeclared = {
		neurus::ShaderPort{"depth", neurus::PortDirection::Output, "d32_sfloat", 0, 0},
	};
	source.Set("GeometryPass",
	           neurus::ExtractPassPorts("GeometryPass", geometryShaders, geometryDeclared));

	const std::array<neurus::ShaderPort, 1> shadowDeclared = {
		neurus::ShaderPort{"shadow_depth", neurus::PortDirection::Output, "logical", 0, 0},
	};
	const std::span<const neurus::ShaderData* const> noShaders;
	source.Set("ShadowDepthPass",
	           neurus::ExtractPassPorts("ShadowDepthPass", noShaders, shadowDeclared));

	auto pointShadow = neurus::ShaderLibrary::LoadComputeShader(
		"shadow_eval", "res/shaders/compute/shadow_eval.comp.glsl");
	auto sunShadow = neurus::ShaderLibrary::LoadComputeShader(
		"sun_shadow_eval", "res/shaders/compute/sun_shadow_eval.comp.glsl");
	EXPECT_NE(pointShadow, nullptr);
	EXPECT_NE(sunShadow, nullptr);
	const std::array<const neurus::ShaderData*, 2> shadowIntensityShaders = {
		pointShadow.get(), sunShadow.get(),
	};
	source.Set("ShadowIntensityPass",
	           neurus::ExtractPassPorts("ShadowIntensityPass", shadowIntensityShaders));

	auto ssao = neurus::ShaderLibrary::LoadComputeShader(
		"ssao", "res/shaders/compute/ssao.comp.glsl");
	auto lighting = neurus::ShaderLibrary::LoadComputeShader(
		"lighting", "res/shaders/compute/pbr_lighting.comp.glsl");
	auto compose = neurus::ShaderLibrary::LoadComputeShader(
		"compose", "res/shaders/compute/compose.comp.glsl");
	auto fxaa = neurus::ShaderLibrary::LoadComputeShader(
		"fxaa", "res/shaders/compute/fxaa.comp.glsl");
	EXPECT_NE(ssao, nullptr);
	EXPECT_NE(lighting, nullptr);
	EXPECT_NE(compose, nullptr);
	EXPECT_NE(fxaa, nullptr);

	const std::array<const neurus::ShaderData*, 1> ssaoShaders = {ssao.get()};
	const std::array<const neurus::ShaderData*, 1> lightingShaders = {lighting.get()};
	const std::array<const neurus::ShaderData*, 1> composeShaders = {compose.get()};
	const std::array<const neurus::ShaderData*, 1> fxaaShaders = {fxaa.get()};

	source.Set("SSAOPass", neurus::ExtractPassPorts("SSAOPass", ssaoShaders));
	source.Set("LightingPass", neurus::ExtractPassPorts("LightingPass", lightingShaders));
	source.Set("ComposePass", neurus::ExtractPassPorts("ComposePass", composeShaders));
	source.Set("FXAAPass", neurus::ExtractPassPorts("FXAAPass", fxaaShaders));

	source.SetExternal("LightingPass", {"U_Irradiance", "U_Prefiltered"});
	return source;
}

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
	const FakePortSource source = MakeRealPortSource();
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
	const FakePortSource source = MakeRealPortSource();
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

	const FakePortSource source = MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "geometry", "NopePass"));
}

TEST(RenderGraphCompile, UnknownPortIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.edges[0].toPort = "bogus";

	const FakePortSource source = MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "ssao", "bogus"));
}

TEST(RenderGraphCompile, WrongDirectionIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.edges[0].from = "ssao";
	spec.edges[0].fromPort = "gPosition";

	const FakePortSource source = MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "ssao", "input, not an output"));
}

TEST(RenderGraphCompile, DoubleProducerIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.edges.push_back({"geometry", "gNormal", "lighting", "gPosition"});

	const FakePortSource source = MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "lighting", "more than one producer"));
}

TEST(RenderGraphCompile, MissingTagIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.FindNode("geometry")->outputs.erase("gPosition");

	const FakePortSource source = MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "geometry", "no tag"));
}

TEST(RenderGraphCompile, TagForUnknownPortIsNamed)
{
	neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();
	spec.FindNode("geometry")->outputs["nope"] =
		{neurus::GraphFormat::Rgba16f, neurus::GraphUsage::ColorSampled};

	const FakePortSource source = MakeRealPortSource();
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

	const FakePortSource source = MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, true);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "lighting", "not connected"));
}

TEST(RenderGraphCompile, ExternalInputIsAccepted)
{
	const FakePortSource source = MakeRealPortSource();
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

	const FakePortSource source = MakeRealPortSource();
	const neurus::CompileResult result = neurus::CompileRenderGraphSpec(spec, source, false);

	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(HasDiagnostic(result, "", "cycle detected"));
	EXPECT_TRUE(HasDiagnostic(result, "", "ssao"));
	EXPECT_TRUE(HasDiagnostic(result, "", "compose"));
}

TEST(RenderGraphCompile, OrderIsDeterministic)
{
	const FakePortSource source = MakeRealPortSource();
	const neurus::RenderGraphSpec spec = neurus::MakeDefaultRenderGraphSpec();

	const neurus::CompileResult first = neurus::CompileRenderGraphSpec(spec, source, true);
	const neurus::CompileResult second = neurus::CompileRenderGraphSpec(spec, source, true);

	ASSERT_TRUE(first.ok) << DiagnosticsText(first);
	ASSERT_TRUE(second.ok) << DiagnosticsText(second);
	EXPECT_EQ(first.order, second.order);
}
