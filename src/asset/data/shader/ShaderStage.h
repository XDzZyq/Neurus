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
inline constexpr const char* kDefaultVertexPath   = "res/shaders/render/gbuffer.vert";
inline constexpr const char* kDefaultFragmentPath = "res/shaders/render/gbuffer.frag";

/**
 * @brief Infers a shader stage from a source-file extension.
 *
 * The comparison is case-insensitive. `.vert`, `.frag`, `.comp` and `.geom`
 * map to their stages; anything else returns std::nullopt.
 *
 * @param path Source path or file name.
 * @return The inferred stage, or std::nullopt for an unknown extension.
 */
inline std::optional<ShaderType> ShaderTypeFromPath(const std::string& path)
{
	const auto dot = path.find_last_of('.');
	if (dot == std::string::npos)
	{
		return std::nullopt;
	}

	std::string ext = path.substr(dot);
	for (char& c : ext)
	{
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}

	if (ext == ".vert") return ShaderType::VERTEX;
	if (ext == ".frag") return ShaderType::FRAGMENT;
	if (ext == ".comp") return ShaderType::COMPUTE;
	if (ext == ".geom") return ShaderType::GEOMETRY;
	return std::nullopt;
}

} // namespace neurus
