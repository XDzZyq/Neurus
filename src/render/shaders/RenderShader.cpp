/**
 * @file RenderShader.cpp
 * @brief Implementation of CPU-only RenderShader with parse->generate flow.
 *
 * Implements the parse->generate pipeline via ShaderParser and ShaderGenerator.
 * All GPU compilation (SPIR-V, ShaderModule creation) is handled externally
 * by ShaderLibrary.
 */

#include "RenderShader.h"

#include "ShaderLibrary.h"
#include "ShaderParser.h"
#include "ShaderGenerator.h"
#include "core/Log.h"

namespace neurus {

// =========================================================================
// Constructor
// =========================================================================

RenderShader::RenderShader(std::string name, std::string vertPath, std::string fragPath)
	: Shader(std::move(name))
	, m_vertPath(std::move(vertPath))
	, m_fragPath(std::move(fragPath))
{
	m_stages[ShaderType::VERTEX]   = ShaderUnit{m_vertPath, {}, {}};
	m_stages[ShaderType::FRAGMENT] = ShaderUnit{m_fragPath, {}, {}};
	NEURUS_LOG("[RenderShader] Created '" << m_name << "'");
}

void RenderShader::ReloadContent()
{
	if (m_vertPath.empty() && m_fragPath.empty())
		return;   // identity shell (empty paths)

	m_stages[ShaderType::VERTEX].path   = m_vertPath;
	m_stages[ShaderType::FRAGMENT].path = m_fragPath;
	ParseAndGenerate();
}

bool RenderShader::CompileToSpv()
{
	bool allOk = true;
	for (auto type : {ShaderType::VERTEX, ShaderType::FRAGMENT})
	{
		if (!HasStage(type)) continue;
		auto& unit = GetStage(type);
		if (!unit.spv.empty()) continue; // idempotent: stage already compiled
		if (unit.code.empty()) continue; // identity shell (no content to compile)

		unit.spv = ShaderLibrary::Compile(unit, type, m_name);
		if (unit.spv.empty())
		{
			NEURUS_ERR("[RenderShader] CompileToSpv failed for '" << m_name
			           << "' stage " << TypeToString(type));
			allOk = false;
			continue;
		}
		unit.BumpVersion();
	}

	if (allOk)
		BumpVersion();
	return allOk;
}

// =========================================================================
// Shader interface - ParseAndGenerate
// =========================================================================

bool RenderShader::ParseAndGenerate()
{
	m_errorMessage.clear();

	// -- Parse vertex --
	{
		auto& vertUnit = m_stages[ShaderType::VERTEX];
		vertUnit.parsed = ShaderParser::ParseShaderFile(vertUnit.path, ShaderType::VERTEX);
		if (vertUnit.parsed.IsEmpty())
		{
			m_errorMessage = "Failed to parse vertex shader: " + vertUnit.path;
			NEURUS_ERR("[RenderShader] " << m_errorMessage);
			return false;
		}
		vertUnit.code = ShaderGenerator::Generate(vertUnit.parsed);
	}

	// -- Parse fragment --
	{
		auto& fragUnit = m_stages[ShaderType::FRAGMENT];
		fragUnit.parsed = ShaderParser::ParseShaderFile(fragUnit.path, ShaderType::FRAGMENT);
		if (fragUnit.parsed.IsEmpty())
		{
			m_errorMessage = "Failed to parse fragment shader: " + fragUnit.path;
			NEURUS_ERR("[RenderShader] " << m_errorMessage);
			return false;
		}
		fragUnit.code = ShaderGenerator::Generate(fragUnit.parsed);
	}

	NEURUS_LOG("[RenderShader] '" << m_name << "' parsed and generated");
	return true;
}

// =========================================================================
// Path accessors
// =========================================================================

const std::string& RenderShader::GetVertPath() const { return m_vertPath; }
const std::string& RenderShader::GetFragPath() const { return m_fragPath; }

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

	auto& unit = m_stages[type];
	unit.parsed = ShaderParser::ParseShaderFile(unit.path, type);
	if (unit.parsed.IsEmpty())
	{
		m_errorMessage = "Failed to re-parse: " + unit.path;
		NEURUS_ERR("[RenderShader] " << m_errorMessage);
		return false;
	}
	unit.code = ShaderGenerator::Generate(unit.parsed);
	NEURUS_LOG("[RenderShader] Recompiled '" << m_name << "' stage " << TypeToString(type));
	return true;
}

} // namespace neurus
