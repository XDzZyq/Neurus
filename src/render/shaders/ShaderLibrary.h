#pragma once

#include "Shader.h"
#include "ShaderStruct.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace neurus {

class Shader;
class RenderShader;
class ComputeShader;
struct ShaderUnit;

class ShaderLibrary
{
public:
	ShaderLibrary() = delete;
	~ShaderLibrary() = delete;
	ShaderLibrary(const ShaderLibrary&) = delete;
	ShaderLibrary& operator=(const ShaderLibrary&) = delete;

	// --- Path resolution ---

	/**
	 * @brief Resolves a shader source path to a readable file location.
	 *
	 * Absolute paths are returned unchanged. Relative "res/shaders/..." paths
	 * are resolved against the compile-time shader directory, with a walk-up
	 * fallback for build-tree working directories, so parsing never depends on
	 * the process working directory while stored paths stay portable.
	 *
	 * @param path Shader source path, relative ("res/shaders/render/x.vert")
	 *             or absolute.
	 * @return Resolved path if a file exists there; otherwise @p path unchanged
	 *         so the caller reports a clear open failure.
	 */
	static std::string ResolveShaderPath(const std::string& path);

	// --- Parse (CPU side, no caching) ---
	static std::unique_ptr<RenderShader> LoadRenderShader(
		const std::string& name,
		const std::string& vertPath,
		const std::string& fragPath);

	static std::unique_ptr<ComputeShader> LoadComputeShader(
		const std::string& name,
		const std::string& compPath);

	// --- Compile (GLSL -> SPIR-V) ---
	static std::vector<uint32_t> Compile(
		const ShaderUnit& stage,
		ShaderType type,
		const std::string& debugName);

	static std::unordered_map<ShaderType, std::vector<uint32_t>> CompileAll(
		const Shader& shader);

	// --- Build-in constants ---
	static const S_Const* GetBuildInConstant(const std::string& name);
};

} // namespace neurus
