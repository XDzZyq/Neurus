/**
 * @file PassKinds.hpp
 * @brief Built-in pass kinds: their shader sources and declared-only ports.
 *
 * The table is Vulkan-free. It is the single description of a built-in pass:
 * the shader pool creates a node's shaders from a row, the default graph seeds
 * its nodes from the same rows, and the passes read their declared outputs and
 * external inputs back from here instead of hard-coding them.
 *
 * A kind lists every shader it owns, because two passes carry a second shader
 * for the sun variant (`ShadowDepthPass`, `ShadowIntensityPass`). Ports are
 * merged over all of them.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace neurus {

/**
 * @brief One shader a pass kind owns.
 *
 * A graphics source names a vertex and a fragment path; a compute source names
 * only @c computePath.
 */
struct PassShaderSource
{
	std::string_view name;
	std::string_view vertexPath;    ///< Empty for a compute source.
	std::string_view fragmentPath;  ///< Empty for a compute source.
	std::string_view computePath;   ///< Empty for a graphics source.
};

/**
 * @brief One port a pass declares without a shader declaration.
 *
 * The G-Buffer depth and the shadow bundle are allocated and ordered by the
 * graph but are not written through a shader binding, so no reflection can
 * see them.
 */
struct DeclaredPort
{
	std::string_view name;
	std::string_view type;   ///< "d32_sfloat", or "logical" for a token.
};

/**
 * @brief One built-in pass kind.
 */
struct PassKindInfo
{
	std::string_view                  name;
	std::span<const PassShaderSource> shaders;    ///< First source is the primary shader.
	std::string_view                  condition;  ///< "fxaa" when the node is conditional.
	std::span<const DeclaredPort>     declaredOutputs;  ///< Outputs with no shader binding.
	std::span<const std::string_view> externalInputs;   ///< Inputs resolved outside the graph.
	std::span<const uint32_t>         optionalBindings; ///< PARTIALLY_BOUND bindings.
};

inline constexpr uint32_t kLightingOptionalBindings[] = {5, 6, 11};

inline constexpr DeclaredPort kGeometryDeclaredOutputs[] = {
	{"depth", "d32_sfloat"},
};

inline constexpr DeclaredPort kShadowDepthDeclaredOutputs[] = {
	{"shadow_depth", "logical"},
};

inline constexpr std::string_view kLightingExternalInputs[] = {
	"U_Irradiance",
	"U_Prefiltered",
};

inline constexpr PassShaderSource kGeometryShaders[] = {
	{
		"GeometryPass",
		"res/shaders/render/gbuffer.vert.glsl",
		"res/shaders/render/gbuffer.frag.glsl",
		"",
	},
};

inline constexpr PassShaderSource kShadowDepthShaders[] = {
	{
		"ShadowDepthMultiview",
		"res/shaders/render/shadow_depth_multiview.vert.glsl",
		"res/shaders/render/shadow_depth.frag.glsl",
		"",
	},
	{
		"ShadowDepthSun",
		"res/shaders/render/sun_shadow_depth.vert.glsl",
		"res/shaders/render/sun_shadow_depth.frag.glsl",
		"",
	},
};

inline constexpr PassShaderSource kShadowIntensityShaders[] = {
	{"shadow_eval", "", "", "res/shaders/compute/shadow_eval.comp.glsl"},
	{"sun_shadow_eval", "", "", "res/shaders/compute/sun_shadow_eval.comp.glsl"},
};

inline constexpr PassShaderSource kSsaoShaders[] = {
	{"ssao", "", "", "res/shaders/compute/ssao.comp.glsl"},
};

inline constexpr PassShaderSource kLightingShaders[] = {
	{"pbr_lighting", "", "", "res/shaders/compute/pbr_lighting.comp.glsl"},
};

inline constexpr PassShaderSource kComposeShaders[] = {
	{"compose", "", "", "res/shaders/compute/compose.comp.glsl"},
};

inline constexpr PassShaderSource kFxaaShaders[] = {
	{"fxaa", "", "", "res/shaders/compute/fxaa.comp.glsl"},
};

/// @brief One row per built-in scene pass.
inline constexpr PassKindInfo kPassKinds[] = {
	{
		"GeometryPass",
		kGeometryShaders,
		{},
		kGeometryDeclaredOutputs,
		{},
		{},
	},
	{
		"ShadowDepthPass",
		kShadowDepthShaders,
		{},
		kShadowDepthDeclaredOutputs,
		{},
		{},
	},
	{
		"ShadowIntensityPass",
		kShadowIntensityShaders,
		{},
		{},
		{},
		{},
	},
	{
		"SSAOPass",
		kSsaoShaders,
		{},
		{},
		{},
		{},
	},
	{
		"LightingPass",
		kLightingShaders,
		{},
		{},
		kLightingExternalInputs,
		kLightingOptionalBindings,
	},
	{
		"ComposePass",
		kComposeShaders,
		{},
		{},
		{},
		{},
	},
	{
		"FXAAPass",
		kFxaaShaders,
		"fxaa",
		{},
		{},
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
