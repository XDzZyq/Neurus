/**
 * @file test_pass_ports.cpp
 * @brief Port extraction from the real pass shaders.
 */

#include <gtest/gtest.h>

#include "asset/data/render_graph/PassPorts.hpp"
#include "shader/ComputeShader.hpp"
#include "shader/RenderShader.hpp"
#include "shader/ShaderLibrary.hpp"

#include <array>
#include <memory>

namespace {

std::unique_ptr<neurus::RenderShader> LoadRender(const char* name,
                                                 const char* vert,
                                                 const char* frag)
{
	auto shader = neurus::ShaderLibrary::LoadRenderShader(name, vert, frag);
	EXPECT_NE(shader, nullptr) << name;
	return shader;
}

std::unique_ptr<neurus::ComputeShader> LoadCompute(const char* name, const char* path)
{
	auto shader = neurus::ShaderLibrary::LoadComputeShader(name, path);
	EXPECT_NE(shader, nullptr) << name;
	return shader;
}

} // anonymous namespace

TEST(PassPorts, TypePredicates)
{
	EXPECT_TRUE(neurus::IsSamplerPortType("sampler2D"));
	EXPECT_TRUE(neurus::IsSamplerPortType("sampler2DArray"));
	EXPECT_TRUE(neurus::IsSamplerPortType("samplerCube"));
	EXPECT_FALSE(neurus::IsSamplerPortType("image2D"));

	EXPECT_TRUE(neurus::IsStorageImagePortType("image2D"));
	EXPECT_TRUE(neurus::IsStorageImagePortType("image2DArray"));
	EXPECT_FALSE(neurus::IsStorageImagePortType("sampler2D"));
}

TEST(PassPorts, GeometryOutputs)
{
	auto shader = LoadRender("GeometryPass",
	                         "res/shaders/render/gbuffer.vert.glsl",
	                         "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const neurus::PassPorts ports = neurus::ExtractPassPorts("GeometryPass", shaders);

	EXPECT_TRUE(ports.problems.empty());
	const auto outputs = ports.Outputs();
	ASSERT_EQ(outputs.size(), 5u);

	const std::vector<std::string> names = {
		"gPosition", "gNormal", "gAlbedo", "gMetallicRoughness", "gObjectID",
	};
	for (size_t i = 0; i < names.size(); ++i)
	{
		EXPECT_EQ(outputs[i].name, names[i]);
		EXPECT_EQ(outputs[i].location, static_cast<uint32_t>(i));
	}
	EXPECT_EQ(outputs[4].type, "uint");
	EXPECT_TRUE(ports.Inputs().empty());
}

TEST(PassPorts, GeometryDeclaredDepth)
{
	auto shader = LoadRender("GeometryPass",
	                         "res/shaders/render/gbuffer.vert.glsl",
	                         "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const std::array<neurus::PassPort, 1> declared = {
		neurus::PassPort{"depth", neurus::PortDirection::Output, "d32_sfloat", 0, 0},
	};
	const neurus::PassPorts ports =
		neurus::ExtractPassPorts("GeometryPass", shaders, declared);

	EXPECT_TRUE(ports.problems.empty());
	ASSERT_EQ(ports.Outputs().size(), 6u);

	const neurus::PassPort* depth =
		ports.Find("depth", neurus::PortDirection::Output);
	ASSERT_NE(depth, nullptr);
	EXPECT_EQ(depth->type, "d32_sfloat");
}

TEST(PassPorts, SsaoPorts)
{
	auto shader = LoadCompute("ssao", "res/shaders/compute/ssao.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const neurus::PassPorts ports = neurus::ExtractPassPorts("SSAOPass", shaders);

	EXPECT_TRUE(ports.problems.empty());
	const auto inputs = ports.Inputs();
	ASSERT_EQ(inputs.size(), 3u);
	EXPECT_EQ(inputs[0].name, "gPosition");
	EXPECT_EQ(inputs[0].binding, 0u);
	EXPECT_EQ(inputs[1].name, "gNormal");
	EXPECT_EQ(inputs[2].name, "gAlpha");

	const auto outputs = ports.Outputs();
	ASSERT_EQ(outputs.size(), 1u);
	EXPECT_EQ(outputs[0].name, "outputSSAO");
	EXPECT_EQ(outputs[0].binding, 3u);
	EXPECT_EQ(outputs[0].type, "image2D");

	EXPECT_EQ(ports.Find("SSAOParams", neurus::PortDirection::Input), nullptr);
	EXPECT_EQ(ports.Find("NoiseBuffer", neurus::PortDirection::Input), nullptr);
}

TEST(PassPorts, LightingPorts)
{
	auto shader = LoadCompute("lighting", "res/shaders/compute/pbr_lighting.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const neurus::PassPorts ports = neurus::ExtractPassPorts("LightingPass", shaders);

	EXPECT_TRUE(ports.problems.empty());
	ASSERT_EQ(ports.Inputs().size(), 8u);
	ASSERT_EQ(ports.Outputs().size(), 1u);

	EXPECT_EQ(ports.Find("gPosition", neurus::PortDirection::Input)->binding, 0u);
	EXPECT_EQ(ports.Find("gNormal", neurus::PortDirection::Input)->binding, 1u);
	EXPECT_EQ(ports.Find("gAlbedo", neurus::PortDirection::Input)->binding, 2u);
	EXPECT_EQ(ports.Find("gMetallicRoughness", neurus::PortDirection::Input)->binding, 3u);
	EXPECT_EQ(ports.Find("U_AO", neurus::PortDirection::Input)->binding, 7u);
	EXPECT_EQ(ports.Find("U_Irradiance", neurus::PortDirection::Input)->binding, 8u);
	EXPECT_EQ(ports.Find("U_Prefiltered", neurus::PortDirection::Input)->binding, 9u);
	EXPECT_EQ(ports.Find("U_ShadowArray", neurus::PortDirection::Input)->binding, 10u);
	EXPECT_EQ(ports.Find("U_ShadowArray", neurus::PortDirection::Input)->type,
	          "sampler2DArray");
	EXPECT_EQ(ports.Find("outputImage", neurus::PortDirection::Output)->binding, 4u);

	EXPECT_EQ(ports.Find("LightBuffer", neurus::PortDirection::Input), nullptr);
	EXPECT_EQ(ports.Find("SunLightBuffer", neurus::PortDirection::Input), nullptr);
	EXPECT_EQ(ports.Find("SpotLightBuffer", neurus::PortDirection::Input), nullptr);
}

TEST(PassPorts, ComposePorts)
{
	auto shader = LoadCompute("compose", "res/shaders/compute/compose.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const neurus::PassPorts ports = neurus::ExtractPassPorts("ComposePass", shaders);

	EXPECT_TRUE(ports.problems.empty());
	ASSERT_EQ(ports.Inputs().size(), 1u);
	ASSERT_EQ(ports.Outputs().size(), 1u);
	EXPECT_EQ(ports.Inputs()[0].name, "gHDRColor");
	EXPECT_EQ(ports.Inputs()[0].binding, 0u);
	EXPECT_EQ(ports.Outputs()[0].name, "outputComposed");
	EXPECT_EQ(ports.Outputs()[0].binding, 1u);
}

TEST(PassPorts, FxaaPorts)
{
	auto shader = LoadCompute("fxaa", "res/shaders/compute/fxaa.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const neurus::PassPorts ports = neurus::ExtractPassPorts("FXAAPass", shaders);

	EXPECT_TRUE(ports.problems.empty());
	ASSERT_EQ(ports.Inputs().size(), 1u);
	ASSERT_EQ(ports.Outputs().size(), 1u);
	EXPECT_EQ(ports.Inputs()[0].name, "gInput");
	EXPECT_EQ(ports.Inputs()[0].binding, 0u);
	EXPECT_EQ(ports.Outputs()[0].name, "outputFXAA");
	EXPECT_EQ(ports.Outputs()[0].binding, 1u);
}

TEST(PassPorts, ShadowIntensityMergesBothShaders)
{
	auto point = LoadCompute("shadow_eval", "res/shaders/compute/shadow_eval.comp.glsl");
	auto sun = LoadCompute("sun_shadow_eval", "res/shaders/compute/sun_shadow_eval.comp.glsl");
	ASSERT_TRUE(point);
	ASSERT_TRUE(sun);

	const std::array<const neurus::ShaderData*, 2> shaders = {point.get(), sun.get()};
	const neurus::PassPorts ports = neurus::ExtractPassPorts("ShadowIntensityPass", shaders);

	EXPECT_TRUE(ports.problems.empty());
	ASSERT_EQ(ports.Inputs().size(), 3u);
	ASSERT_EQ(ports.Outputs().size(), 1u);

	EXPECT_EQ(ports.Find("gPosition", neurus::PortDirection::Input)->binding, 0u);
	EXPECT_EQ(ports.Find("u_ShadowCube", neurus::PortDirection::Input)->binding, 1u);
	EXPECT_EQ(ports.Find("u_SunShadowMap", neurus::PortDirection::Input)->binding, 1u);
	EXPECT_EQ(ports.Find("outputShadow", neurus::PortDirection::Output)->binding, 2u);
	EXPECT_EQ(ports.Find("outputShadow", neurus::PortDirection::Output)->type,
	          "image2DArray");
}

TEST(PassPorts, ConflictingBindingIsReported)
{
	neurus::ComputeShader first("first");
	first.GetStage(neurus::ShaderType::COMPUTE).parsed.SetUni(
		neurus::ParaType::CUSTOM, 1, "a", 0, "", "sampler2D", "");

	neurus::ComputeShader second("second");
	second.GetStage(neurus::ShaderType::COMPUTE).parsed.SetUni(
		neurus::ParaType::CUSTOM, 1, "a", 1, "", "sampler2D", "");

	const std::array<const neurus::ShaderData*, 2> shaders = {&first, &second};
	const neurus::PassPorts ports = neurus::ExtractPassPorts("ConflictPass", shaders);

	EXPECT_EQ(ports.ports.size(), 1u);
	ASSERT_EQ(ports.problems.size(), 1u);
	EXPECT_NE(ports.problems[0].find("a"), std::string::npos);
}
