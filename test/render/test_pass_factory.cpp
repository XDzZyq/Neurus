/**
 * @file test_pass_factory.cpp
 * @brief Built-in and custom pass construction.
 */

#include <gtest/gtest.h>
#include "asset/data/render_graph/PortSource.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "shader/ShaderLibrary.hpp"
#include "shader/ComputeShader.hpp"
#include "render/passes/Pass.hpp"
#include "render/RenderCache.hpp"
#include "render/UploadManager.hpp"

#include "shared/TestVulkanShared.hpp"

#include "render/render_graph/PassFactory.hpp"

#include <string>

using namespace neurus;

class PassFactoryTest : public VulkanTestShared
{
};

TEST_F(PassFactoryTest, AllSevenKindsExist)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const auto defaults = std::make_shared<const BuiltInRenderData>();
	RenderCache cache(*m_device, PhysicalDevice(), defaults);
	PassFactory factory(*m_device, PhysicalDevice(), 2, *m_queue, m_graphicsQueueFamily, cache);

	EXPECT_NE(factory.Find("GeometryPass"), nullptr);
	EXPECT_NE(factory.Find("LightingPass"), nullptr);
	EXPECT_NE(factory.Find("SSAOPass"), nullptr);
	EXPECT_NE(factory.Find("ShadowDepthPass"), nullptr);
	EXPECT_NE(factory.Find("ShadowIntensityPass"), nullptr);
	EXPECT_NE(factory.Find("ComposePass"), nullptr);
	EXPECT_NE(factory.Find("FXAAPass"), nullptr);

	EXPECT_EQ(factory.Find("NopePass"), nullptr);
}

TEST(BuiltInRenderDataTest, PortsComeFromTheShaders)
{
	const BuiltInRenderData defaults;
	const IPortSource& catalog = defaults.Ports();

	const PassPorts* geometry = catalog.FindPassPorts("GeometryPass");
	ASSERT_NE(geometry, nullptr);
	EXPECT_EQ(geometry->Outputs().size(), 6u);  // five fragment outputs + depth
	EXPECT_TRUE(geometry->Inputs().empty());
	ASSERT_NE(geometry->Find("depth", PortDirection::Output), nullptr);

	const PassPorts* ssao = catalog.FindPassPorts("SSAOPass");
	ASSERT_NE(ssao, nullptr);
	EXPECT_EQ(ssao->Inputs().size(), 3u);
	EXPECT_EQ(ssao->Outputs().size(), 1u);

	const PassPorts* lighting = catalog.FindPassPorts("LightingPass");
	ASSERT_NE(lighting, nullptr);
	EXPECT_EQ(lighting->Inputs().size(), 8u);
	EXPECT_EQ(lighting->Outputs().size(), 1u);

	const std::vector<std::string> externals = catalog.ExternalInputs("LightingPass");
	ASSERT_EQ(externals.size(), 2u);
	EXPECT_EQ(externals[0], "U_Irradiance");
	EXPECT_EQ(externals[1], "U_Prefiltered");

	const PassPorts* shadowIntensity = catalog.FindPassPorts("ShadowIntensityPass");
	ASSERT_NE(shadowIntensity, nullptr);
	EXPECT_EQ(shadowIntensity->Inputs().size(), 3u);
	EXPECT_EQ(shadowIntensity->Outputs().size(), 1u);

	const PassPorts* shadowDepth = catalog.FindPassPorts("ShadowDepthPass");
	ASSERT_NE(shadowDepth, nullptr);
	EXPECT_EQ(shadowDepth->Outputs().size(), 1u);
	EXPECT_EQ(shadowDepth->Outputs()[0].name, "shadow_depth");
}

TEST_F(PassFactoryTest, CustomNodeInstancesCacheOnlyWhileOwned)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	const auto defaults = std::make_shared<const BuiltInRenderData>();
	RenderCache cache(*m_device, PhysicalDevice(), defaults);
	PassFactory factory(*m_device, PhysicalDevice(), 2, *m_queue, m_graphicsQueueFamily, cache);
	std::shared_ptr<neurus::ComputeShader> shader = neurus::ShaderLibrary::LoadComputeShader("custom compose", "res/shaders/compute/compose.comp.glsl");
	ASSERT_NE(shader, nullptr);
	m_resources.Register(shader);
	ASSERT_TRUE(shader->CompileToSpv());
	const int shaderId = shader->RequireObjectID();
	// UID zero is valid; absence is represented by nullopt.
	auto builtin = factory.Acquire("ComposePass", "first", std::nullopt);
	auto otherBuiltin = factory.Acquire("ComposePass", "second", std::nullopt);
	ASSERT_NE(builtin, nullptr);
	ASSERT_NE(otherBuiltin, nullptr);
	EXPECT_NE(builtin->GraphDescriptorSet(0), otherBuiltin->GraphDescriptorSet(0));
	EXPECT_EQ(builtin, factory.Acquire("ComposePass", "first", std::nullopt));
	cache.UseShader(neurus::UploadManager::UploadShader(*m_device, neurus::MakeUploadShaderRequest(*shader)));
	auto first = factory.Acquire("ComposePass", "first", shaderId);
	auto same = factory.Acquire("ComposePass", "first", shaderId);
	auto second = factory.Acquire("ComposePass", "second", shaderId);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(first, same);
	EXPECT_NE(first, second);
	EXPECT_NE(first->GraphDescriptorSet(0), second->GraphDescriptorSet(0));
	const auto generation = cache.ShaderGeneration();
	cache.UseShader(neurus::UploadManager::UploadShader(*m_device, neurus::MakeUploadShaderRequest(*shader)));
	EXPECT_GT(cache.ShaderGeneration(), generation);
	auto replacement = factory.Acquire("ComposePass", "first", shaderId);
	EXPECT_NE(replacement, first);
	std::weak_ptr<neurus::Pass> old = first;
	first.reset();
	same.reset();
	EXPECT_TRUE(old.expired());
	EXPECT_NE(factory.Acquire("ComposePass", "first", shaderId), nullptr);
}
