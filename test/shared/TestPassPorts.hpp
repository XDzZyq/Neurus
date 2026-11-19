/**
 * @file TestPassPorts.hpp
 * @brief A port source over the real shaders, shared by the graph tests.
 */

#pragma once

#include <gtest/gtest.h>

#include "asset/data/render_graph/PortSource.hpp"

#include "asset/data/shader/ComputeShader.hpp"
#include "asset/data/shader/RenderShader.hpp"
#include "asset/shader/ShaderLibrary.hpp"

#include <array>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neurus::test {

/**
 * @brief A port source a test fills with the kinds it needs.
 */
class FakePortSource : public IPortSource
{
public:
	void Set(std::string kind, PassPorts ports)
	{
		m_ports[std::move(kind)] = std::move(ports);
	}

	void SetExternal(std::string kind, std::vector<std::string> names)
	{
		m_externals[std::move(kind)] = std::move(names);
	}

	const PassPorts* FindPassPorts(const std::string_view kind) const override
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
	std::unordered_map<std::string, PassPorts>                m_ports;
	std::unordered_map<std::string, std::vector<std::string>> m_externals;
};

/**
 * @brief Loads the real built-in shaders and extracts their ports.
 */
inline FakePortSource MakeRealPortSource()
{
	FakePortSource source;

	auto geometry = ShaderLibrary::LoadRenderShader(
		"GeometryPass",
		"res/shaders/render/gbuffer.vert.glsl",
		"res/shaders/render/gbuffer.frag.glsl");
	EXPECT_NE(geometry, nullptr);

	const std::array<const ShaderData*, 1> geometryShaders = {geometry.get()};
	const std::array<PassPort, 1> geometryDeclared = {
		PassPort{"depth", PortDirection::Output, "d32_sfloat", 0, 0},
	};
	source.Set("GeometryPass",
	           ExtractPassPorts("GeometryPass", geometryShaders, geometryDeclared));

	const std::array<PassPort, 1> shadowDeclared = {
		PassPort{"shadow_depth", PortDirection::Output, "logical", 0, 0},
	};
	const std::span<const ShaderData* const> noShaders;
	source.Set("ShadowDepthPass",
	           ExtractPassPorts("ShadowDepthPass", noShaders, shadowDeclared));

	auto pointShadow = ShaderLibrary::LoadComputeShader(
		"shadow_eval", "res/shaders/compute/shadow_eval.comp.glsl");
	auto sunShadow = ShaderLibrary::LoadComputeShader(
		"sun_shadow_eval", "res/shaders/compute/sun_shadow_eval.comp.glsl");
	EXPECT_NE(pointShadow, nullptr);
	EXPECT_NE(sunShadow, nullptr);
	const std::array<const ShaderData*, 2> shadowIntensityShaders = {
		pointShadow.get(), sunShadow.get(),
	};
	source.Set("ShadowIntensityPass",
	           ExtractPassPorts("ShadowIntensityPass", shadowIntensityShaders));

	auto ssao = ShaderLibrary::LoadComputeShader(
		"ssao", "res/shaders/compute/ssao.comp.glsl");
	auto lighting = ShaderLibrary::LoadComputeShader(
		"lighting", "res/shaders/compute/pbr_lighting.comp.glsl");
	auto compose = ShaderLibrary::LoadComputeShader(
		"compose", "res/shaders/compute/compose.comp.glsl");
	auto fxaa = ShaderLibrary::LoadComputeShader(
		"fxaa", "res/shaders/compute/fxaa.comp.glsl");
	EXPECT_NE(ssao, nullptr);
	EXPECT_NE(lighting, nullptr);
	EXPECT_NE(compose, nullptr);
	EXPECT_NE(fxaa, nullptr);

	const std::array<const ShaderData*, 1> ssaoShaders = {ssao.get()};
	const std::array<const ShaderData*, 1> lightingShaders = {lighting.get()};
	const std::array<const ShaderData*, 1> composeShaders = {compose.get()};
	const std::array<const ShaderData*, 1> fxaaShaders = {fxaa.get()};

	source.Set("SSAOPass", ExtractPassPorts("SSAOPass", ssaoShaders));
	source.Set("LightingPass", ExtractPassPorts("LightingPass", lightingShaders));
	source.Set("ComposePass", ExtractPassPorts("ComposePass", composeShaders));
	source.Set("FXAAPass", ExtractPassPorts("FXAAPass", fxaaShaders));

	source.SetExternal("LightingPass", {"U_Irradiance", "U_Prefiltered"});
	return source;
}

} // namespace neurus::test
