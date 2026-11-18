/**
 * @file test_shader_snapshot.cpp
 * @brief Shader snapshot and pass-kind table.
 */

#include <gtest/gtest.h>

#include "asset/data/render_graph/PassKinds.hpp"
#include "render/RenderFrameSnapshot.hpp"
#include "render/shaders/ComputeShader.hpp"
#include "render/shaders/RenderShader.hpp"
#include "render/shaders/ShaderLibrary.hpp"

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

const neurus::PushConstantMember* FindPush(const neurus::ShaderSnapshot& snapshot,
                                           const std::string& name)
{
	for (const auto& member : snapshot.pushConstants)
	{
		if (member.name == name)
			return &member;
	}
	return nullptr;
}

const neurus::DescriptorBinding* FindBinding(const neurus::ShaderSnapshot& snapshot,
                                             const uint32_t binding)
{
	for (const auto& reflected : snapshot.bindings)
	{
		if (reflected.binding == binding)
			return &reflected;
	}
	return nullptr;
}

} // anonymous namespace

TEST(ShaderSnapshot, GeometryPorts)
{
	auto shader = LoadRender("GeometryPass",
	                         "res/shaders/render/gbuffer.vert.glsl",
	                         "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_TRUE(shader);

	const neurus::ShaderSnapshot snapshot = neurus::MakeShaderSnapshot(*shader);
	ASSERT_EQ(snapshot.ports.size(), 5u);
	EXPECT_EQ(snapshot.ports[0].name, "gPosition");
	EXPECT_EQ(snapshot.ports[0].location, 0u);
	EXPECT_EQ(snapshot.ports[4].name, "gObjectID");
	EXPECT_EQ(snapshot.ports[4].type, "uint");
}

TEST(ShaderSnapshot, LightingPortsBindingsAndPushConstants)
{
	auto shader = LoadCompute("lighting", "res/shaders/compute/pbr_lighting.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<uint32_t, 3> optional = {5, 6, 11};
	const neurus::ShaderSnapshot snapshot = neurus::MakeShaderSnapshot(*shader, optional);

	const auto inputs = std::count_if(snapshot.ports.begin(), snapshot.ports.end(),
		[](const neurus::PassPort& port)
		{
			return port.direction == neurus::PortDirection::Input;
		});
	EXPECT_EQ(inputs, 8);
	const auto output = std::find_if(snapshot.ports.begin(), snapshot.ports.end(),
		[](const neurus::PassPort& port)
		{
			return port.name == "outputImage";
		});
	ASSERT_NE(output, snapshot.ports.end());
	EXPECT_EQ(output->binding, 4u);

	ASSERT_EQ(snapshot.bindings.size(), 12u);
	EXPECT_EQ(FindBinding(snapshot, 0)->kind, neurus::DescriptorKind::CombinedImageSampler);
	EXPECT_EQ(FindBinding(snapshot, 4)->kind, neurus::DescriptorKind::StorageImage);
	EXPECT_EQ(FindBinding(snapshot, 5)->kind, neurus::DescriptorKind::StorageBuffer);
	EXPECT_TRUE(FindBinding(snapshot, 5)->partiallyBound);
	EXPECT_TRUE(FindBinding(snapshot, 6)->partiallyBound);
	EXPECT_TRUE(FindBinding(snapshot, 11)->partiallyBound);
	EXPECT_FALSE(FindBinding(snapshot, 0)->partiallyBound);

	const neurus::PushConstantMember* camera = FindPush(snapshot, "cameraPos");
	ASSERT_NE(camera, nullptr);
	EXPECT_EQ(camera->offset, 16u);
	EXPECT_EQ(camera->size, 16u);

	const neurus::PushConstantMember* invProjView = FindPush(snapshot, "invProjView");
	ASSERT_NE(invProjView, nullptr);
	EXPECT_EQ(invProjView->offset, 112u);
	EXPECT_EQ(invProjView->size, 64u);
}

TEST(ShaderSnapshot, SsaoPushConstants)
{
	auto shader = LoadCompute("ssao", "res/shaders/compute/ssao.comp.glsl");
	ASSERT_TRUE(shader);

	const neurus::ShaderSnapshot snapshot = neurus::MakeShaderSnapshot(*shader);
	ASSERT_EQ(snapshot.pushConstants.size(), 4u);

	EXPECT_EQ(snapshot.pushConstants[0].name, "kernelLength");
	EXPECT_EQ(snapshot.pushConstants[0].offset, 0u);
	EXPECT_EQ(snapshot.pushConstants[1].offset, 4u);
	EXPECT_EQ(snapshot.pushConstants[2].offset, 8u);
	EXPECT_EQ(snapshot.pushConstants[3].offset, 12u);
}

TEST(ShaderSnapshot, FxaaPushConstants)
{
	auto shader = LoadCompute("fxaa", "res/shaders/compute/fxaa.comp.glsl");
	ASSERT_TRUE(shader);

	const neurus::ShaderSnapshot snapshot = neurus::MakeShaderSnapshot(*shader);

	const neurus::PushConstantMember* rcp = FindPush(snapshot, "rcpFrame");
	ASSERT_NE(rcp, nullptr);
	EXPECT_EQ(rcp->offset, 0u);
	EXPECT_EQ(rcp->size, 16u);

	EXPECT_EQ(FindPush(snapshot, "qualitySubpix")->offset, 16u);
	EXPECT_EQ(FindPush(snapshot, "edgeThreshold")->offset, 20u);
	EXPECT_EQ(FindPush(snapshot, "edgeThresholdMin")->offset, 24u);
}

TEST(ShaderSnapshot, PassKindTable)
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

TEST(ShaderSnapshot, PortRenameBreaksEdge)
{
	auto shader = LoadRender("GeometryPass",
	                         "res/shaders/render/gbuffer.vert.glsl",
	                         "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_TRUE(shader);

	for (neurus::S_IO& output : shader->GetFragment().parsed.pass_list)
		output.name = "renamed_" + output.name;

	const neurus::ShaderSnapshot snapshot = neurus::MakeShaderSnapshot(*shader);
	for (const neurus::PassPort& port : snapshot.ports)
		EXPECT_EQ(port.name.rfind("renamed_", 0), 0u) << port.name;
}
