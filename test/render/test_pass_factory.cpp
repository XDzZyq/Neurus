/**
 * @file test_pass_factory.cpp
 * @brief Built-in pass construction and port cache.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.hpp"

#include "render/render_graph/PassFactory.hpp"

#include <string>
#include <stdexcept>

using namespace neurus;

class PassFactoryTest : public VulkanTestShared
{
};

TEST_F(PassFactoryTest, AllSevenKindsExist)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	PassFactory factory(*m_device, PhysicalDevice(), 2, *m_queue, m_graphicsQueueFamily);

	EXPECT_NE(factory.Find("GeometryPass"), nullptr);
	EXPECT_NE(factory.Find("LightingPass"), nullptr);
	EXPECT_NE(factory.Find("SSAOPass"), nullptr);
	EXPECT_NE(factory.Find("ShadowDepthPass"), nullptr);
	EXPECT_NE(factory.Find("ShadowIntensityPass"), nullptr);
	EXPECT_NE(factory.Find("ComposePass"), nullptr);
	EXPECT_NE(factory.Find("FXAAPass"), nullptr);

	EXPECT_EQ(factory.Find("NopePass"), nullptr);
	EXPECT_EQ(factory.FindPassPorts("NopePass"), nullptr);
	EXPECT_THROW((void)factory.Ports("NopePass"), std::invalid_argument);
}

TEST_F(PassFactoryTest, PortsComeFromTheShaders)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	PassFactory factory(*m_device, PhysicalDevice(), 2, *m_queue, m_graphicsQueueFamily);

	const PassPorts* geometry = factory.FindPassPorts("GeometryPass");
	ASSERT_NE(geometry, nullptr);
	EXPECT_EQ(geometry->Outputs().size(), 6u);  // five fragment outputs + depth
	EXPECT_TRUE(geometry->Inputs().empty());
	ASSERT_NE(geometry->Find("depth", PortDirection::Output), nullptr);

	const PassPorts* ssao = factory.FindPassPorts("SSAOPass");
	ASSERT_NE(ssao, nullptr);
	EXPECT_EQ(ssao->Inputs().size(), 3u);
	EXPECT_EQ(ssao->Outputs().size(), 1u);

	const PassPorts* lighting = factory.FindPassPorts("LightingPass");
	ASSERT_NE(lighting, nullptr);
	EXPECT_EQ(lighting->Inputs().size(), 8u);
	EXPECT_EQ(lighting->Outputs().size(), 1u);

	const std::vector<std::string> externals = factory.ExternalInputs("LightingPass");
	ASSERT_EQ(externals.size(), 2u);
	EXPECT_EQ(externals[0], "U_Irradiance");
	EXPECT_EQ(externals[1], "U_Prefiltered");

	const PassPorts* shadowIntensity = factory.FindPassPorts("ShadowIntensityPass");
	ASSERT_NE(shadowIntensity, nullptr);
	EXPECT_EQ(shadowIntensity->Inputs().size(), 3u);
	EXPECT_EQ(shadowIntensity->Outputs().size(), 1u);

	const PassPorts* shadowDepth = factory.FindPassPorts("ShadowDepthPass");
	ASSERT_NE(shadowDepth, nullptr);
	EXPECT_EQ(shadowDepth->Outputs().size(), 1u);
	EXPECT_EQ(shadowDepth->Outputs()[0].name, "shadow_depth");
}
