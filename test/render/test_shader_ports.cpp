/**
 * @file test_shader_ports.cpp
 * @brief CPU shader ports, parsed layout and pass-kind table.
 */

#include <gtest/gtest.h>

#include "asset/data/render_graph/PassKinds.hpp"
#include "asset/data/render_graph/PassPorts.hpp"
#include "shader/ComputeShader.hpp"
#include "shader/RenderShader.hpp"
#include "shader/ShaderLibrary.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

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

neurus::PassPorts Ports(const neurus::ShaderData& shader)
{
	const std::array<const neurus::ShaderData*, 1> shaders = {&shader};
	return neurus::ExtractPassPorts(shader.GetName(), shaders);
}

const neurus::S_PushConstant* FindPush(const neurus::ShaderStruct& ir, const std::string& name)
{
	for (const auto& member : ir.push_constants)
		if (member.name == name)
			return &member;
	return nullptr;
}

} // anonymous namespace

TEST(ShaderPorts, GeometryPorts)
{
	auto shader = LoadRender("GeometryPass",
	                         "res/shaders/render/gbuffer.vert.glsl",
	                         "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_TRUE(shader);

	const auto ports = Ports(*shader);
	ASSERT_EQ(ports.ports.size(), 5u);
	EXPECT_EQ(ports.ports[0].name, "gPosition");
	EXPECT_EQ(ports.ports[0].location, 0u);
	EXPECT_EQ(ports.ports[4].name, "gObjectID");
	EXPECT_EQ(ports.ports[4].type, "uint");
}

TEST(ShaderPorts, LightingPortsBindingsAndPushConstants)
{
	auto shader = LoadCompute("lighting", "res/shaders/compute/pbr_lighting.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<uint32_t, 3> optional = {5, 6, 11};
	const auto ports = Ports(*shader);
	const auto& ir = shader->GetStage(neurus::ShaderType::COMPUTE).parsed;

	const auto inputs = std::count_if(ports.ports.begin(), ports.ports.end(),
		[](const neurus::PassPort& port)
		{
			return port.direction == neurus::PortDirection::Input;
		});
	EXPECT_EQ(inputs, 8);
	const auto output = std::find_if(ports.ports.begin(), ports.ports.end(),
		[](const neurus::PassPort& port)
		{
			return port.name == "outputImage";
		});
	ASSERT_NE(output, ports.ports.end());
	EXPECT_EQ(output->binding, 4u);

	const neurus::S_PushConstant* camera = FindPush(ir, "cameraPos");
	ASSERT_NE(camera, nullptr);
	EXPECT_EQ(camera->offset, 16u);
	EXPECT_EQ(camera->size, 16u);

	const neurus::S_PushConstant* invProjView = FindPush(ir, "invProjView");
	ASSERT_NE(invProjView, nullptr);
	EXPECT_EQ(invProjView->offset, 112u);
	EXPECT_EQ(invProjView->size, 64u);
}

TEST(ShaderPorts, SsaoPushConstants)
{
	auto shader = LoadCompute("ssao", "res/shaders/compute/ssao.comp.glsl");
	ASSERT_TRUE(shader);

	const auto ports = Ports(*shader);
	const auto& ir = shader->GetStage(neurus::ShaderType::COMPUTE).parsed;
	ASSERT_EQ(ir.push_constants.size(), 4u);

	EXPECT_EQ(ir.push_constants[0].name, "kernelLength");
	EXPECT_EQ(ir.push_constants[0].offset, 0u);
	EXPECT_EQ(ir.push_constants[1].offset, 4u);
	EXPECT_EQ(ir.push_constants[2].offset, 8u);
	EXPECT_EQ(ir.push_constants[3].offset, 12u);
}

TEST(ShaderPorts, FxaaPushConstants)
{
	auto shader = LoadCompute("fxaa", "res/shaders/compute/fxaa.comp.glsl");
	ASSERT_TRUE(shader);

	const auto ports = Ports(*shader);
	const auto& ir = shader->GetStage(neurus::ShaderType::COMPUTE).parsed;

	const neurus::S_PushConstant* rcp = FindPush(ir, "rcpFrame");
	ASSERT_NE(rcp, nullptr);
	EXPECT_EQ(rcp->offset, 0u);
	EXPECT_EQ(rcp->size, 16u);

	EXPECT_EQ(FindPush(ir, "qualitySubpix")->offset, 16u);
	EXPECT_EQ(FindPush(ir, "edgeThreshold")->offset, 20u);
	EXPECT_EQ(FindPush(ir, "edgeThresholdMin")->offset, 24u);
}

TEST(ShaderPorts, PassKindTable)
{
	const neurus::PassKindInfo* lighting = neurus::FindPassKind("LightingPass");
	ASSERT_NE(lighting, nullptr);
	ASSERT_EQ(lighting->shaders.size(), 1u);
	EXPECT_EQ(lighting->shaders[0].computePath, "res/shaders/compute/pbr_lighting.comp.glsl");
	EXPECT_EQ(lighting->optionalBindings.size(), 3u);

	const neurus::PassKindInfo* shadowDepth = neurus::FindPassKind("ShadowDepthPass");
	ASSERT_NE(shadowDepth, nullptr);
	ASSERT_EQ(shadowDepth->shaders.size(), 2u);  // multiview cubemap and sun 2D
	EXPECT_EQ(shadowDepth->shaders[0].name, "ShadowDepthMultiview");
	EXPECT_EQ(shadowDepth->shaders[1].name, "ShadowDepthSun");

	const neurus::PassKindInfo* shadowIntensity = neurus::FindPassKind("ShadowIntensityPass");
	ASSERT_NE(shadowIntensity, nullptr);
	EXPECT_EQ(shadowIntensity->shaders.size(), 2u);

	EXPECT_NE(neurus::FindPassKind("GeometryPass"), nullptr);
	EXPECT_NE(neurus::FindPassKind("SSAOPass"), nullptr);
	EXPECT_NE(neurus::FindPassKind("ComposePass"), nullptr);
	EXPECT_NE(neurus::FindPassKind("FXAAPass"), nullptr);
	EXPECT_EQ(neurus::FindPassKind("NopePass"), nullptr);
}

TEST(ShaderPorts, PortRenameBreaksEdge)
{
	auto shader = LoadRender("GeometryPass",
	                         "res/shaders/render/gbuffer.vert.glsl",
	                         "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_TRUE(shader);

	for (neurus::S_IO& output : shader->GetFragment().parsed.pass_list)
		output.name = "renamed_" + output.name;

	const auto ports = Ports(*shader);
	for (const neurus::PassPort& port : ports.ports)
		EXPECT_EQ(port.name.rfind("renamed_", 0), 0u) << port.name;
}
