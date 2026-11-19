/**
 * @file test_shader_reflection.cpp
 * @brief Descriptor layout and push-constant reflection from the real shaders.
 */

#include <gtest/gtest.h>

#include "render/render_graph/ShaderReflection.hpp"
#include "asset/data/shader/ComputeShader.hpp"
#include "asset/data/shader/RenderShader.hpp"
#include "asset/shader/ShaderLibrary.hpp"

#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using neurus::ReflectedDescriptorLayout;

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

const vk::DescriptorSetLayoutBinding* FindBinding(const ReflectedDescriptorLayout& layout,
                                                  const uint32_t binding)
{
	for (const vk::DescriptorSetLayoutBinding& candidate : layout.bindings)
	{
		if (candidate.binding == binding)
			return &candidate;
	}
	return nullptr;
}

vk::DescriptorBindingFlags FlagsFor(const ReflectedDescriptorLayout& layout,
                                    const uint32_t binding)
{
	for (size_t i = 0; i < layout.bindings.size(); ++i)
	{
		if (layout.bindings[i].binding == binding)
			return layout.bindingFlags[i];
	}
	return {};
}

void ExpectBinding(const ReflectedDescriptorLayout& layout,
                   const uint32_t binding,
                   const vk::DescriptorType type,
                   const vk::ShaderStageFlags stages,
                   const uint32_t count = 1)
{
	const vk::DescriptorSetLayoutBinding* found = FindBinding(layout, binding);
	ASSERT_NE(found, nullptr) << "binding " << binding;
	EXPECT_EQ(found->descriptorType, type) << "binding " << binding;
	EXPECT_EQ(found->descriptorCount, count) << "binding " << binding;
	EXPECT_EQ(found->stageFlags, stages) << "binding " << binding;
}

constexpr vk::ShaderStageFlags kCompute  = vk::ShaderStageFlagBits::eCompute;
constexpr vk::ShaderStageFlags kVertex   = vk::ShaderStageFlagBits::eVertex;
constexpr vk::ShaderStageFlags kFragment = vk::ShaderStageFlagBits::eFragment;

} // anonymous namespace

TEST(ShaderReflection, GeometryLayout)
{
	auto shader = LoadRender("GeometryPass",
	                         "res/shaders/render/gbuffer.vert.glsl",
	                         "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const ReflectedDescriptorLayout layout = neurus::ReflectDescriptorLayout(shaders);

	ASSERT_EQ(layout.bindings.size(), 1u);
	ASSERT_EQ(layout.bindingFlags.size(), layout.bindings.size());
	ExpectBinding(layout, 0, vk::DescriptorType::eUniformBuffer, kVertex);
}

TEST(ShaderReflection, SsaoLayout)
{
	auto shader = LoadCompute("ssao", "res/shaders/compute/ssao.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const ReflectedDescriptorLayout layout = neurus::ReflectDescriptorLayout(shaders);

	ASSERT_EQ(layout.bindings.size(), 6u);
	ExpectBinding(layout, 0, vk::DescriptorType::eCombinedImageSampler, kCompute);
	ExpectBinding(layout, 1, vk::DescriptorType::eCombinedImageSampler, kCompute);
	ExpectBinding(layout, 2, vk::DescriptorType::eCombinedImageSampler, kCompute);
	ExpectBinding(layout, 3, vk::DescriptorType::eStorageImage, kCompute);
	ExpectBinding(layout, 4, vk::DescriptorType::eUniformBuffer, kCompute);
	ExpectBinding(layout, 5, vk::DescriptorType::eUniformBuffer, kCompute);
}

TEST(ShaderReflection, LightingLayout)
{
	auto shader = LoadCompute("lighting", "res/shaders/compute/pbr_lighting.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const std::array<uint32_t, 3> optional = {5, 6, 11};
	const ReflectedDescriptorLayout layout =
		neurus::ReflectDescriptorLayout(shaders, optional);

	ASSERT_EQ(layout.bindings.size(), 12u);

	for (uint32_t binding = 0; binding <= 3; ++binding)
		ExpectBinding(layout, binding, vk::DescriptorType::eCombinedImageSampler, kCompute);

	ExpectBinding(layout, 4, vk::DescriptorType::eStorageImage, kCompute);
	ExpectBinding(layout, 5, vk::DescriptorType::eStorageBuffer, kCompute);
	ExpectBinding(layout, 6, vk::DescriptorType::eStorageBuffer, kCompute);

	for (uint32_t binding = 7; binding <= 10; ++binding)
		ExpectBinding(layout, binding, vk::DescriptorType::eCombinedImageSampler, kCompute);

	ExpectBinding(layout, 11, vk::DescriptorType::eStorageBuffer, kCompute);

	EXPECT_NE(FlagsFor(layout, 5) & vk::DescriptorBindingFlagBits::ePartiallyBound,
	          vk::DescriptorBindingFlags{});
	EXPECT_NE(FlagsFor(layout, 6) & vk::DescriptorBindingFlagBits::ePartiallyBound,
	          vk::DescriptorBindingFlags{});
	EXPECT_NE(FlagsFor(layout, 11) & vk::DescriptorBindingFlagBits::ePartiallyBound,
	          vk::DescriptorBindingFlags{});
	EXPECT_EQ(FlagsFor(layout, 0), vk::DescriptorBindingFlags{});
	EXPECT_EQ(FlagsFor(layout, 4), vk::DescriptorBindingFlags{});
}

TEST(ShaderReflection, ComposeLayout)
{
	auto shader = LoadCompute("compose", "res/shaders/compute/compose.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const ReflectedDescriptorLayout layout = neurus::ReflectDescriptorLayout(shaders);

	ASSERT_EQ(layout.bindings.size(), 2u);
	ExpectBinding(layout, 0, vk::DescriptorType::eCombinedImageSampler, kCompute);
	ExpectBinding(layout, 1, vk::DescriptorType::eStorageImage, kCompute);
}

TEST(ShaderReflection, FxaaLayout)
{
	auto shader = LoadCompute("fxaa", "res/shaders/compute/fxaa.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const ReflectedDescriptorLayout layout = neurus::ReflectDescriptorLayout(shaders);

	ASSERT_EQ(layout.bindings.size(), 2u);
	ExpectBinding(layout, 0, vk::DescriptorType::eCombinedImageSampler, kCompute);
	ExpectBinding(layout, 1, vk::DescriptorType::eStorageImage, kCompute);
}

TEST(ShaderReflection, ShadowIntensityMergesBothShaders)
{
	auto point = LoadCompute("shadow_eval", "res/shaders/compute/shadow_eval.comp.glsl");
	auto sun = LoadCompute("sun_shadow_eval", "res/shaders/compute/sun_shadow_eval.comp.glsl");
	ASSERT_TRUE(point);
	ASSERT_TRUE(sun);

	const std::array<const neurus::ShaderData*, 2> shaders = {point.get(), sun.get()};
	const ReflectedDescriptorLayout layout = neurus::ReflectDescriptorLayout(shaders);

	ASSERT_EQ(layout.bindings.size(), 3u);
	ExpectBinding(layout, 0, vk::DescriptorType::eCombinedImageSampler, kCompute);
	ExpectBinding(layout, 1, vk::DescriptorType::eCombinedImageSampler, kCompute);
	ExpectBinding(layout, 2, vk::DescriptorType::eStorageImage, kCompute);
}

TEST(ShaderReflection, ShadowDepthLayout)
{
	auto shader = LoadRender("ShadowDepthMultiview",
	                         "res/shaders/render/shadow_depth_multiview.vert.glsl",
	                         "res/shaders/render/shadow_depth.frag.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const ReflectedDescriptorLayout layout = neurus::ReflectDescriptorLayout(shaders);

	ASSERT_EQ(layout.bindings.size(), 1u);
	ExpectBinding(layout, 0, vk::DescriptorType::eStorageBuffer, kVertex);
}

TEST(ShaderReflection, GeometryPushRange)
{
	auto shader = LoadRender("GeometryPass",
	                         "res/shaders/render/gbuffer.vert.glsl",
	                         "res/shaders/render/gbuffer.frag.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const vk::PushConstantRange range = neurus::PushConstantRangeFromShaders(
		shaders, kVertex | kFragment);

	EXPECT_EQ(range.offset, 0u);
	EXPECT_EQ(range.size, 132u);
}

TEST(ShaderReflection, SsaoPushRange)
{
	auto shader = LoadCompute("ssao", "res/shaders/compute/ssao.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const vk::PushConstantRange range = neurus::PushConstantRangeFromShaders(shaders, kCompute);

	EXPECT_EQ(range.offset, 0u);
	EXPECT_EQ(range.size, 16u);
}

TEST(ShaderReflection, LightingPushRange)
{
	auto shader = LoadCompute("lighting", "res/shaders/compute/pbr_lighting.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const vk::PushConstantRange range = neurus::PushConstantRangeFromShaders(shaders, kCompute);

	EXPECT_EQ(range.offset, 0u);
	EXPECT_EQ(range.size, 176u);
}

TEST(ShaderReflection, ComposePushRange)
{
	auto shader = LoadCompute("compose", "res/shaders/compute/compose.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const vk::PushConstantRange range = neurus::PushConstantRangeFromShaders(shaders, kCompute);

	EXPECT_EQ(range.size, 4u);
}

TEST(ShaderReflection, FxaaPushRange)
{
	auto shader = LoadCompute("fxaa", "res/shaders/compute/fxaa.comp.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const vk::PushConstantRange range = neurus::PushConstantRangeFromShaders(shaders, kCompute);

	EXPECT_EQ(range.size, 28u);
}

TEST(ShaderReflection, ShadowIntensityPushRanges)
{
	auto point = LoadCompute("shadow_eval", "res/shaders/compute/shadow_eval.comp.glsl");
	auto sun = LoadCompute("sun_shadow_eval", "res/shaders/compute/sun_shadow_eval.comp.glsl");
	ASSERT_TRUE(point);
	ASSERT_TRUE(sun);

	const std::array<const neurus::ShaderData*, 1> pointShaders = {point.get()};
	const std::array<const neurus::ShaderData*, 1> sunShaders = {sun.get()};

	const vk::PushConstantRange pointRange =
		neurus::PushConstantRangeFromShaders(pointShaders, kCompute);
	const vk::PushConstantRange sunRange =
		neurus::PushConstantRangeFromShaders(sunShaders, kCompute);

	EXPECT_EQ(pointRange.size, 48u);
	EXPECT_EQ(sunRange.size, 96u);
}

TEST(ShaderReflection, ShadowDepthPushRange)
{
	auto shader = LoadRender("ShadowDepthMultiview",
	                         "res/shaders/render/shadow_depth_multiview.vert.glsl",
	                         "res/shaders/render/shadow_depth.frag.glsl");
	ASSERT_TRUE(shader);

	const std::array<const neurus::ShaderData*, 1> shaders = {shader.get()};
	const vk::PushConstantRange range = neurus::PushConstantRangeFromShaders(
		shaders, kVertex | kFragment);

	EXPECT_EQ(range.size, 80u);
}

TEST(ShaderReflection, ConflictingBindingThrows)
{
	neurus::ComputeShader shader("conflict");
	shader.GetStage(neurus::ShaderType::COMPUTE).parsed.SetUni(
		neurus::ParaType::CUSTOM, 1, "a", 0, "", "sampler2D", "");
	shader.GetStage(neurus::ShaderType::COMPUTE).parsed.SetSB(0, "B", {});

	const std::array<const neurus::ShaderData*, 1> shaders = {&shader};

	try
	{
		(void)neurus::ReflectDescriptorLayout(shaders);
		FAIL() << "expected a conflicting-binding exception";
	}
	catch (const std::runtime_error& error)
	{
		EXPECT_NE(std::string(error.what()).find("binding 0"), std::string::npos);
	}
}
