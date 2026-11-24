/**
 * @file test_pass_bindings.cpp
 * @brief Binding-plan construction from a pass's ports.
 */

#include <gtest/gtest.h>

#include "render/passes/Pass.hpp"
#include "render/render_graph/PassBindings.hpp"

#include <string>
#include <string_view>
#include <vector>

using namespace neurus;

namespace {

class SelfManagedPass : public Pass
{
public:
	std::vector<uint32_t> SelfManagedBindings() const override { return {1}; }
};

class PlainPass : public Pass
{
};

} // anonymous namespace

TEST(PassBindings, BuildsImageInputsAndOutputs)
{
	PassPorts ports;
	ports.ports = {
		{"selfManaged", PortDirection::Input, "sampler2D", 1, 0},
		{"gIn",   PortDirection::Input,  "sampler2D", 0, 0},
		{"gOut",  PortDirection::Output, "image2D",   2, 0},
		{"gFrag", PortDirection::Output, "vec4",      0, 0},
		{"gBuf",  PortDirection::Input,  "uniform",   3, 0},
	};

	SelfManagedPass pass;
	const std::vector<GraphImageBinding> bindings =
		BuildImageBindings(ports, pass);

	ASSERT_EQ(bindings.size(), 2u);

	EXPECT_EQ(bindings[0].port, "gIn");
	EXPECT_EQ(bindings[0].direction, PortDirection::Input);
	EXPECT_EQ(bindings[0].binding, 0u);
	EXPECT_EQ(bindings[0].type, vk::DescriptorType::eCombinedImageSampler);
	EXPECT_EQ(bindings[0].layout, vk::ImageLayout::eShaderReadOnlyOptimal);

	EXPECT_EQ(bindings[1].port, "gOut");
	EXPECT_EQ(bindings[1].direction, PortDirection::Output);
	EXPECT_EQ(bindings[1].binding, 2u);
	EXPECT_EQ(bindings[1].type, vk::DescriptorType::eStorageImage);
	EXPECT_EQ(bindings[1].layout, vk::ImageLayout::eGeneral);
}

TEST(PassBindings, ConnectedPredicateFiltersInputs)
{
	PassPorts ports;
	ports.ports = {
		{"gIn",  PortDirection::Input,  "sampler2D", 0, 0},
		{"gOut", PortDirection::Output, "image2D",   2, 0},
	};

	SelfManagedPass pass;
	const std::vector<GraphImageBinding> bindings =
		BuildImageBindings(ports, pass,
		                   [](const std::string_view port) { return port == "other"; });

	ASSERT_EQ(bindings.size(), 1u);
	EXPECT_EQ(bindings[0].port, "gOut");
	EXPECT_EQ(bindings[0].direction, PortDirection::Output);
}

TEST(PassBindings, DefaultPassHasNoSelfManagedBindings)
{
	PassPorts ports;
	ports.ports = {
		{"gIn", PortDirection::Input, "sampler2D", 1, 0},
	};

	PlainPass pass;
	const std::vector<GraphImageBinding> bindings =
		BuildImageBindings(ports, pass);

	ASSERT_EQ(bindings.size(), 1u);
	EXPECT_EQ(bindings[0].port, "gIn");
}
