/**
 * @file test_pass_registry.cpp
 * @brief Built-in pass construction and port cache.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.hpp"

#include "render/render_graph/PassRegistry.hpp"

#include <string>
#include <stdexcept>

using namespace neurus;

class PassRegistryTest : public VulkanTestShared
{
};

TEST_F(PassRegistryTest, AllSevenKindsExist)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	PassRegistry registry(*m_device, PhysicalDevice(), 2, *m_queue, m_graphicsQueueFamily);

	EXPECT_NE(registry.Find("GeometryPass"), nullptr);
	EXPECT_NE(registry.Find("LightingPass"), nullptr);
	EXPECT_NE(registry.Find("SSAOPass"), nullptr);
	EXPECT_NE(registry.Find("ShadowDepthPass"), nullptr);
	EXPECT_NE(registry.Find("ShadowIntensityPass"), nullptr);
	EXPECT_NE(registry.Find("ComposePass"), nullptr);
	EXPECT_NE(registry.Find("FXAAPass"), nullptr);

	EXPECT_EQ(registry.Find("NopePass"), nullptr);
	EXPECT_EQ(registry.FindPassPorts("NopePass"), nullptr);
	EXPECT_THROW((void)registry.Ports("NopePass"), std::invalid_argument);
}

TEST_F(PassRegistryTest, PortsComeFromTheShaders)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	PassRegistry registry(*m_device, PhysicalDevice(), 2, *m_queue, m_graphicsQueueFamily);

	const PassPorts* geometry = registry.FindPassPorts("GeometryPass");
	ASSERT_NE(geometry, nullptr);
	EXPECT_EQ(geometry->Outputs().size(), 6u);  // five fragment outputs + depth
	EXPECT_TRUE(geometry->Inputs().empty());
	ASSERT_NE(geometry->Find("depth", PortDirection::Output), nullptr);

	const PassPorts* ssao = registry.FindPassPorts("SSAOPass");
	ASSERT_NE(ssao, nullptr);
	EXPECT_EQ(ssao->Inputs().size(), 3u);
	EXPECT_EQ(ssao->Outputs().size(), 1u);

	const PassPorts* lighting = registry.FindPassPorts("LightingPass");
	ASSERT_NE(lighting, nullptr);
	EXPECT_EQ(lighting->Inputs().size(), 8u);
	EXPECT_EQ(lighting->Outputs().size(), 1u);

	const std::vector<std::string> externals = registry.ExternalInputs("LightingPass");
	ASSERT_EQ(externals.size(), 2u);
	EXPECT_EQ(externals[0], "U_Irradiance");
	EXPECT_EQ(externals[1], "U_Prefiltered");

	const PassPorts* shadowIntensity = registry.FindPassPorts("ShadowIntensityPass");
	ASSERT_NE(shadowIntensity, nullptr);
	EXPECT_EQ(shadowIntensity->Inputs().size(), 3u);
	EXPECT_EQ(shadowIntensity->Outputs().size(), 1u);

	const PassPorts* shadowDepth = registry.FindPassPorts("ShadowDepthPass");
	ASSERT_NE(shadowDepth, nullptr);
	EXPECT_EQ(shadowDepth->Outputs().size(), 1u);
	EXPECT_EQ(shadowDepth->Outputs()[0].name, "shadow_depth");
}
