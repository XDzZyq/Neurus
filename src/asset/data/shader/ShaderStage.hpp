#pragma once

#include <cctype>
#include <optional>
#include <string>

namespace neurus
{

/**
 * @brief Shader stage types supported by the renderer.
 */
enum class ShaderType
{
	VERTEX = 0,   ///< Vertex shader stage
	FRAGMENT = 1, ///< Fragment (pixel) shader stage
	COMPUTE = 2,  ///< Compute shader stage (GPGPU)
	GEOMETRY = 3  ///< Geometry shader stage
};

/// @brief Default render-shader pair used when a stage is missing.
inline constexpr const char* kDefaultVertexPath   = "res/shaders/render/gbuffer.vert.glsl";
inline constexpr const char* kDefaultFragmentPath = "res/shaders/render/gbuffer.frag.glsl";

/**
 * @brief Infers a shader stage from a source-file name.
 *
 * The comparison is case-insensitive. `.vert.glsl`, `.frag.glsl`, `.comp.glsl`
 * and `.geom.glsl` map to their stages; the trailing `.glsl` is optional, so the
 * short `.vert` / `.frag` / `.comp` / `.geom` forms are accepted too. Anything
 * else returns std::nullopt.
 *
 * @param path Source path or file name.
 * @return The inferred stage, or std::nullopt for an unknown extension.
 */
inline std::optional<ShaderType> ShaderTypeFromPath(const std::string& path)
{
	std::string name = path;
	for (char& c : name)
	{
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}

	// Strip the GLSL source suffix so the stage extension is the last one.
	if (name.ends_with(".glsl"))
	{
		name.resize(name.size() - (sizeof(".glsl") - 1));
	}

	const auto dot = name.find_last_of('.');
	if (dot == std::string::npos)
	{
		return std::nullopt;
	}

	const std::string ext = name.substr(dot);
	if (ext == ".vert") return ShaderType::VERTEX;
	if (ext == ".frag") return ShaderType::FRAGMENT;
	if (ext == ".comp") return ShaderType::COMPUTE;
	if (ext == ".geom") return ShaderType::GEOMETRY;
	return std::nullopt;
}

} // namespace neurus
