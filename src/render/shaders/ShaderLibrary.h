#pragma once

#include "asset/data/shader/ShaderData.h"
#include "asset/data/shader/ShaderStruct.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace neurus {

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
	 * Absolute paths are returned unchanged. A relative path is tried against
	 * NEURUS_SHADER_DIR first - an absolute path baked in at configure time, so it
	 * names a location in the build machine's tree - then against "../../../",
	 * "../../" and "../", and only then against the process working directory.
	 * Two consequences follow: an ancestor or build-tree copy of the file shadows
	 * a working-directory-relative one, and the working-directory independence
	 * this buys holds for the build tree, not for a binary relocated away from it.
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
		const std::string& debugName,
		std::string* outError = nullptr);

	static std::unordered_map<ShaderType, std::vector<uint32_t>> CompileAll(
		const ShaderData& shader);

	// --- Build-in constants ---
	static const S_Const* GetBuildInConstant(const std::string& name);
};

} // namespace neurus
