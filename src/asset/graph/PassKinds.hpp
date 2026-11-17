/**
 * @file PassKinds.hpp
 * @brief Built-in pass kinds and the shader sources each one starts from.
 *
 * The table is Vulkan-free: it names the default sources and the bindings a
 * pass marks PARTIALLY_BOUND. The floating shader pool can create a new node
 * from a row, and the default graph seeds its nodes from the same rows.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace neurus {

/**
 * @brief One built-in pass kind.
 */
struct PassKindInfo
{
	std::string_view          name;
	std::string_view          vertexPath;    ///< Empty for a compute pass.
	std::string_view          fragmentPath;  ///< Empty for a compute pass.
	std::string_view          computePath;   ///< Empty for a graphics pass.
	std::span<const uint32_t> optionalBindings;  ///< PARTIALLY_BOUND bindings.
};

inline constexpr uint32_t kLightingOptionalBindings[] = {5, 6, 11};

/// @brief One row per built-in scene pass.
inline constexpr PassKindInfo kPassKinds[] = {
	{
		"GeometryPass",
		"res/shaders/render/gbuffer.vert.glsl",
		"res/shaders/render/gbuffer.frag.glsl",
		"",
		{},
	},
	{
		"ShadowDepthPass",
		"res/shaders/render/shadow_depth_multiview.vert.glsl",
		"res/shaders/render/shadow_depth.frag.glsl",
		"",
		{},
	},
	{
		"ShadowIntensityPass",
		"",
		"",
		"res/shaders/compute/shadow_eval.comp.glsl",
		{},
	},
	{
		"SSAOPass",
		"",
		"",
		"res/shaders/compute/ssao.comp.glsl",
		{},
	},
	{
		"LightingPass",
		"",
		"",
		"res/shaders/compute/pbr_lighting.comp.glsl",
		kLightingOptionalBindings,
	},
	{
		"ComposePass",
		"",
		"",
		"res/shaders/compute/compose.comp.glsl",
		{},
	},
	{
		"FXAAPass",
		"",
		"",
		"res/shaders/compute/fxaa.comp.glsl",
		{},
	},
};

/**
 * @brief Finds a built-in pass kind by name.
 * @return Non-owning row, or nullptr when the kind is unknown.
 */
inline const PassKindInfo* FindPassKind(const std::string_view name)
{
	for (const PassKindInfo& kind : kPassKinds)
	{
		if (kind.name == name)
			return &kind;
	}
	return nullptr;
}

} // namespace neurus
