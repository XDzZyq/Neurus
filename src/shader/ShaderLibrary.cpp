#include "shader/ShaderLibrary.hpp"

#include "asset/data/ShaderData.hpp"
#include "shader/ShaderCompiler.hpp"
#include "shader/RenderShader.hpp"
#include "shader/ComputeShader.hpp"
#include "shader/ShaderGenerator.hpp"
#include "shader/ShaderParser.hpp"

#include "core/Log.hpp"

#include <shaderc/shaderc.hpp>

namespace neurus {

// =========================================================================
// Reuse one compiler per calling thread; compilation diagnostics are mutable.
// =========================================================================

static ShaderCompiler& GetCompiler()
{
	static thread_local ShaderCompiler s_compiler;
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
	return ShaderData::ResolveSourcePath(path);
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

std::optional<ShaderUnit> ShaderLibrary::LoadStageUnit(const std::string& path, ShaderType type,
                                                       std::string* outError)
{
	const auto inferred = ShaderTypeFromPath(path);
	if (!inferred || *inferred != type)
	{
		if (outError != nullptr)
		{
			*outError = "shader path is not a " + ShaderData::TypeToString(type) + " file: " + path;
		}
		return std::nullopt;
	}

	ShaderUnit unit;
	unit.path = path;
	unit.parsed = ShaderParser::ParseShaderFile(path, type);
	if (unit.parsed.IsEmpty())
	{
		if (outError != nullptr) *outError = "failed to parse shader: " + path;
		return std::nullopt;
	}
	unit.code = ShaderGenerator::Generate(unit.parsed);
	return unit;
}

std::vector<uint32_t> ShaderLibrary::Compile(
	const ShaderUnit& stage,
	ShaderType type,
	const std::string& debugName,
	std::string* outError)
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

	auto spv = compiler.CompileGlslToSpv(stage.code, kind, "main", debugName);
	if (spv.empty() && outError != nullptr)
	{
		*outError = compiler.GetErrorMessage();
	}
	return spv;
}

std::unordered_map<ShaderType, std::vector<uint32_t>> ShaderLibrary::CompileAll(
	const ShaderData& shader)
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
