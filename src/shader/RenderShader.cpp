/**
 * @file RenderShader.cpp
 * @brief Implementation of CPU-only RenderShader with parse->generate flow.
 *
 * Implements the parse->generate pipeline via ShaderParser and ShaderGenerator.
 * ShaderLibrary compiles generated GLSL to SPIR-V on the CPU. Vulkan shader
 * modules and GPU pipelines remain renderer-owned.
 */

#include "shader/RenderShader.hpp"

#include "shader/ShaderLibrary.hpp"
#include "shader/ShaderInterface.hpp"
#include "shader/ShaderParser.hpp"
#include "shader/ShaderGenerator.hpp"
#include "core/Log.hpp"

namespace neurus {

// =========================================================================
// Constructor
// =========================================================================

RenderShader::RenderShader(std::string name, std::string vertPath, std::string fragPath)
	: ShaderData(std::move(name))
	, sh_vertPath(std::move(vertPath))
	, sh_fragPath(std::move(fragPath))
{
	sh_stages[ShaderType::VERTEX]   = ShaderUnit{sh_vertPath, {}, {}};
	sh_stages[ShaderType::FRAGMENT] = ShaderUnit{sh_fragPath, {}, {}};
	NEURUS_LOG("[RenderShader] Created '" << sh_name << "'");
}

RenderShader::RenderShader(std::string name, ShaderUnit vert, ShaderUnit frag)
	: ShaderData(std::move(name))
	, sh_vertPath(vert.path)
	, sh_fragPath(frag.path)
{
	sh_stages[ShaderType::VERTEX]   = std::move(vert);
	sh_stages[ShaderType::FRAGMENT] = std::move(frag);
	NEURUS_LOG("[RenderShader] Created '" << sh_name << "' from resolved stages");
}

void RenderShader::ReloadContent()
{
	if (sh_vertPath.empty() && sh_fragPath.empty())
		return;   // identity shell (empty paths)

	sh_stages[ShaderType::VERTEX].path   = sh_vertPath;
	sh_stages[ShaderType::FRAGMENT].path = sh_fragPath;
	ParseAndGenerate();
}

bool RenderShader::CompileToSpv(std::string* outError)
{
	sh_errorMessage.clear();
	if (outError != nullptr) outError->clear();
	bool allOk = true;
	for (auto type : {ShaderType::VERTEX, ShaderType::FRAGMENT})
	{
		if (!HasStage(type)) continue;
		auto& unit = GetStage(type);
		if (!unit.spv.empty()) continue; // idempotent: stage already compiled
		if (unit.code.empty()) continue; // identity shell (no content to compile)

		std::string stageError;
		unit.spv = ShaderLibrary::Compile(unit, type, sh_name, &stageError);
		if (unit.spv.empty())
		{
			NEURUS_ERR("[RenderShader] CompileToSpv failed for '" << sh_name
			           << "' stage " << TypeToString(type));
			if (outError != nullptr && outError->empty())
			{
				*outError = stageError.empty() ? "shader compile failed" : stageError;
			}
			allOk = false;
			continue;
		}
		unit.BumpVersion();
	}

	if (allOk && !GetVertex().spv.empty() && !GetFragment().spv.empty())
	{
		allOk = ValidateRenderShaderInterface(GetVertex().spv, GetFragment().spv, sh_errorMessage);
		if (!allOk)
		{
			NEURUS_ERR("[RenderShader] '" << sh_name << "': " << sh_errorMessage);
			if (outError != nullptr) *outError = sh_errorMessage;
		}
	}
	if (!allOk)
	{
		// A partially compiled or incompatible pair must never appear GPU-ready.
		GetVertex().spv.clear();
		GetFragment().spv.clear();
	}
	else
		BumpVersion();
	return allOk;
}

// =========================================================================
// Shader interface - ParseAndGenerate
// =========================================================================

bool RenderShader::ParseAndGenerate()
{
	sh_errorMessage.clear();

	// -- Parse vertex --
	{
		auto& vertUnit = sh_stages[ShaderType::VERTEX];
		vertUnit.parsed = ShaderParser::ParseShaderFile(vertUnit.path, ShaderType::VERTEX);
		if (vertUnit.parsed.IsEmpty())
		{
			sh_errorMessage = "Failed to parse vertex shader: " + vertUnit.path;
			NEURUS_ERR("[RenderShader] " << sh_errorMessage);
			return false;
		}
		vertUnit.code = ShaderGenerator::Generate(vertUnit.parsed);
	}

	// -- Parse fragment --
	{
		auto& fragUnit = sh_stages[ShaderType::FRAGMENT];
		fragUnit.parsed = ShaderParser::ParseShaderFile(fragUnit.path, ShaderType::FRAGMENT);
		if (fragUnit.parsed.IsEmpty())
		{
			sh_errorMessage = "Failed to parse fragment shader: " + fragUnit.path;
			NEURUS_ERR("[RenderShader] " << sh_errorMessage);
			return false;
		}
		fragUnit.code = ShaderGenerator::Generate(fragUnit.parsed);
	}

	NEURUS_LOG("[RenderShader] '" << sh_name << "' parsed and generated");
	return true;
}

// =========================================================================
// Path accessors
// =========================================================================

const std::string& RenderShader::GetVertPath() const { return sh_vertPath; }
const std::string& RenderShader::GetFragPath() const { return sh_fragPath; }

// =========================================================================
// Recompile
// =========================================================================

bool RenderShader::Recompile(ShaderType type)
{
	if (type != ShaderType::VERTEX && type != ShaderType::FRAGMENT)
	{
		NEURUS_ERR("[RenderShader] Recompile: unsupported type");
		return false;
	}

	auto& unit = sh_stages[type];
	unit.parsed = ShaderParser::ParseShaderFile(unit.path, type);
	if (unit.parsed.IsEmpty())
	{
		sh_errorMessage = "Failed to re-parse: " + unit.path;
		NEURUS_ERR("[RenderShader] " << sh_errorMessage);
		return false;
	}
	unit.code = ShaderGenerator::Generate(unit.parsed);
	NEURUS_LOG("[RenderShader] Recompiled '" << sh_name << "' stage " << TypeToString(type));
	return true;
}

} // namespace neurus
