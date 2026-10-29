#include "ShaderLibrary.h"

#include "Shader.h"
#include "ShaderCompiler.h"
#include "RenderShader.h"
#include "ComputeShader.h"

#include "core/Log.h"

#include <shaderc/shaderc.hpp>

#include <filesystem>

namespace neurus {

// =========================================================================
// Meyer's singleton - internal state via function-local statics
// =========================================================================

static ShaderCompiler& GetCompiler()
{
	static ShaderCompiler s_compiler;
	return s_compiler;
}

// =========================================================================
// Build-in constants (ported from OpenGL ShaderBuildIn.cpp)
// =========================================================================

static const std::unordered_map<std::string, S_Const>& GetBuildInConstantsInstance()
{
	static const std::unordered_map<std::string, S_Const> s_constants = {
		// Mathematical constants
		{
			"B_PI",
			{
				ParaType::FLOAT,           // returnType
				"B_PI",                    // name
				"3.141592653589",          // body (GLSL expression)
				{}                          // args (empty = constant, not a function)
			}
		},
		{
			"B_PIX_UV_RATIO",
			{
				ParaType::FLOAT,
				"B_PIX_UV_RATIO",
				"1.0 / textureSize(select_texture, 0)",
				{}
			}
		},

		// Colour-space / tone-mapping placeholders (empty bodies)
		{
			"B_Gamma",
			{
				ParaType::VEC3,
				"B_Gamma",
				"",
				{}
			}
		},
		{
			"B_FilmicF",
			{
				ParaType::VEC3,
				"B_FilmicF",
				"",
				{}
			}
		},
		{
			"B_FilmicV4",
			{
				ParaType::VEC3,
				"B_FilmicV4",
				"",
				{}
			}
		},
	};
	return s_constants;
}

// =========================================================================
// Public API - path resolution
// =========================================================================

std::string ShaderLibrary::ResolveShaderPath(const std::string& path)
{
	// Already absolute? (Unix /, Windows \, or X: drive)
	if (!path.empty()
		&& (path[0] == '/' || path[0] == '\\'
			|| (path.size() > 1 && path[1] == ':')))
	{
		return path;
	}

	// Strip a leading "res/shaders/" prefix if present, so that paths like
	// "res/shaders/compute/foo.comp" resolve correctly against NEURUS_SHADER_DIR
	// (which already points to .../res/shaders/).  Without this, concatenation
	// produces a doubled ".../res/shaders/res/shaders/..." path.
	std::string relPath = path;
	{
		constexpr const char* kShaderPrefix = "res/shaders/";
		constexpr size_t kShaderPrefixLen = 12;
		if (relPath.size() > kShaderPrefixLen
			&& relPath.compare(0, kShaderPrefixLen, kShaderPrefix) == 0)
		{
			relPath.erase(0, kShaderPrefixLen);
		}
	}

	// Try relative to the compile-time shader directory
	{
		std::string resolved = NEURUS_SHADER_DIR + relPath;
		if (std::filesystem::exists(resolved))
		{
			NEURUS_LOG("[ShaderLibrary] Resolved '" << path << "' -> '" << resolved << "'");
			return resolved;
		}
	}

	// Fallback: walk up directories (tests run from build/debug/test/)
	for (const auto& prefix : {"../../../", "../../", "../", ""})
	{
		std::string resolved = std::string(prefix) + path;
		if (std::filesystem::exists(resolved))
		{
			NEURUS_LOG("[ShaderLibrary] Resolved '" << path << "' -> '" << resolved << "'");
			return resolved;
		}
	}

	// Could not resolve - return original and let the caller fail with a clear error
	NEURUS_LOG("[ShaderLibrary] Could NOT resolve shader path '" << path << "'");
	return path;
}

// =========================================================================
// Public API - shader loading
// =========================================================================

std::unique_ptr<RenderShader> ShaderLibrary::LoadRenderShader(
	const std::string& name,
	const std::string& vertPath,
	const std::string& fragPath)
{
	auto resolvedVert = ResolveShaderPath(vertPath);
	auto resolvedFrag = ResolveShaderPath(fragPath);

	auto shader = std::make_unique<RenderShader>(name, resolvedVert, resolvedFrag);
	if (!shader->ParseAndGenerate())
	{
		NEURUS_ERR("[ShaderLibrary] Failed to parse render shader '" << name << "'");
		return nullptr;
	}
	NEURUS_LOG("[ShaderLibrary] Parsed render shader '" << name << "'");
	return shader;
}

std::unique_ptr<ComputeShader> ShaderLibrary::LoadComputeShader(
	const std::string& name,
	const std::string& compPath)
{
	auto resolvedComp = ResolveShaderPath(compPath);

	auto shader = std::make_unique<ComputeShader>(name, resolvedComp);
	if (!shader->ParseAndGenerate())
	{
		NEURUS_ERR("[ShaderLibrary] Failed to parse compute shader '" << name << "'");
		return nullptr;
	}
	NEURUS_LOG("[ShaderLibrary] Parsed compute shader '" << name << "'");
	return shader;
}

std::vector<uint32_t> ShaderLibrary::Compile(
	const ShaderUnit& stage,
	ShaderType type,
	const std::string& debugName)
{
	auto& compiler = GetCompiler();
	shaderc_shader_kind kind;
	switch (type)
	{
		case ShaderType::VERTEX:   kind = shaderc_glsl_vertex_shader; break;
		case ShaderType::FRAGMENT: kind = shaderc_glsl_fragment_shader; break;
		case ShaderType::COMPUTE:  kind = shaderc_glsl_compute_shader; break;
		case ShaderType::GEOMETRY: kind = shaderc_glsl_geometry_shader; break;
		default:                   kind = shaderc_glsl_vertex_shader;
	}

	return compiler.CompileGlslToSpv(stage.code, kind, "main", debugName);
}

std::unordered_map<ShaderType, std::vector<uint32_t>> ShaderLibrary::CompileAll(
	const Shader& shader)
{
	std::unordered_map<ShaderType, std::vector<uint32_t>> results;

	if (shader.GetType() == ShaderType::VERTEX)
	{
		results[ShaderType::VERTEX] = Compile(
			shader.GetStage(ShaderType::VERTEX), ShaderType::VERTEX, shader.GetName() + "_vert");
		results[ShaderType::FRAGMENT] = Compile(
			shader.GetStage(ShaderType::FRAGMENT), ShaderType::FRAGMENT, shader.GetName() + "_frag");
	}
	else if (shader.GetType() == ShaderType::COMPUTE)
	{
		results[ShaderType::COMPUTE] = Compile(
			shader.GetStage(ShaderType::COMPUTE), ShaderType::COMPUTE, shader.GetName());
	}

	return results;
}

const S_Const* ShaderLibrary::GetBuildInConstant(const std::string& name)
{
	const auto& constants = GetBuildInConstantsInstance();
	auto it = constants.find(name);
	return (it != constants.end()) ? &it->second : nullptr;
}

} // namespace neurus
