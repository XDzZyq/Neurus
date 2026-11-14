/**
 * @file ComputeShader.cpp
 * @brief Implementation of the CPU-only parse->generate pipeline for compute shaders.
 */

#include "ComputeShader.h"

#include "ShaderGenerator.h"
#include "ShaderLibrary.h"
#include "ShaderParser.h"

#include "core/Log.h"

#include <algorithm>

namespace neurus {

// =========================================================================
// Construction
// =========================================================================

ComputeShader::ComputeShader(std::string name, std::string compPath)
	: ShaderData(std::move(name))
	, m_compPath(std::move(compPath))
{
	m_stages[ShaderType::COMPUTE] = ShaderUnit{m_compPath, {}, {}};
	NEURUS_LOG("[ComputeShader] Created '" << m_name << "'");
}

// =========================================================================
// Shader interface
// =========================================================================

bool ComputeShader::ParseAndGenerate()
{
	m_errorMessage.clear();

	auto& unit = m_stages[ShaderType::COMPUTE];
	unit.parsed = ShaderParser::ParseShaderFile(unit.path, ShaderType::COMPUTE);
	if (unit.parsed.IsEmpty())
	{
		m_errorMessage = "Failed to parse compute shader: " + unit.path;
		NEURUS_ERR("[ComputeShader] " << m_errorMessage);
		return false;
	}
	unit.code = ShaderGenerator::Generate(unit.parsed);

	NEURUS_LOG("[ComputeShader] '" << m_name << "' parsed and generated");
	return true;
}

void ComputeShader::ReloadContent()
{
	if (m_compPath.empty()) return;

	m_stages[ShaderType::COMPUTE].path = m_compPath;
	ParseAndGenerate();
}

bool ComputeShader::CompileToSpv()
{
	auto& unit = m_stages[ShaderType::COMPUTE];
	if (!unit.spv.empty()) return true;
	if (unit.code.empty()) return false;

	unit.spv = ShaderLibrary::Compile(unit, ShaderType::COMPUTE, m_name);
	if (unit.spv.empty())
	{
		NEURUS_ERR("[ComputeShader] CompileToSpv failed for '" << m_name << "'");
		return false;
	}
	unit.BumpVersion();
	BumpVersion();
	return true;
}

// =========================================================================
// Compute-specific API
// =========================================================================

void ComputeShader::GetWorkgroupSize(uint32_t& x, uint32_t& y, uint32_t& z) const
{
	const auto& s = m_stages.at(ShaderType::COMPUTE).parsed;
	x = s.local_size_x;
	y = s.local_size_y;
	z = s.local_size_z;
}

bool ComputeShader::Recompile()
{
	auto& unit = m_stages[ShaderType::COMPUTE];
	unit.parsed = ShaderParser::ParseShaderFile(unit.path, ShaderType::COMPUTE);
	if (unit.parsed.IsEmpty())
	{
		m_errorMessage = "Failed to re-parse: " + unit.path;
		NEURUS_ERR("[ComputeShader] " << m_errorMessage);
		return false;
	}
	unit.code = ShaderGenerator::Generate(unit.parsed);
	return true;
}

// =========================================================================
// Default uniform configurations (ported from OpenGL ComputeShader)
// =========================================================================

void ComputeShader::SetDefault(const std::string& name, int value)
{
	m_defaults.erase(
		std::remove_if(m_defaults.begin(), m_defaults.end(),
			[&](const Default& d) { return d.name == name; }),
		m_defaults.end());
	m_defaults.push_back({name, "int", std::to_string(value)});
}

void ComputeShader::SetDefault(const std::string& name, float value)
{
	m_defaults.erase(
		std::remove_if(m_defaults.begin(), m_defaults.end(),
			[&](const Default& d) { return d.name == name; }),
		m_defaults.end());
	m_defaults.push_back({name, "float", std::to_string(value)});
}

void ComputeShader::SetDefault(const std::string& name, bool value)
{
	m_defaults.erase(
		std::remove_if(m_defaults.begin(), m_defaults.end(),
			[&](const Default& d) { return d.name == name; }),
		m_defaults.end());
	m_defaults.push_back({name, "bool", value ? "true" : "false"});
}

} // namespace neurus
