/**
 * @file ComputeShader.cpp
 * @brief Implementation of the CPU-only parse->generate pipeline for compute shaders.
 */

#include "shader/ComputeShader.hpp"

#include "shader/ShaderGenerator.hpp"
#include "shader/ShaderLibrary.hpp"
#include "shader/ShaderParser.hpp"

#include "core/Log.hpp"

#include <algorithm>

namespace neurus {

// =========================================================================
// Construction
// =========================================================================

ComputeShader::ComputeShader(RestoredId id)
	: ShaderData(id)
{
	sh_stages.try_emplace(ShaderType::COMPUTE);
}

ComputeShader::ComputeShader(std::string name, std::string compPath)
	: ShaderData(std::move(name))
	, sh_compPath(std::move(compPath))
{
	sh_stages[ShaderType::COMPUTE] = ShaderUnit{sh_compPath, {}, {}};
	NEURUS_LOG("[ComputeShader] Created '" << sh_name << "'");
}

// =========================================================================
// Shader interface
// =========================================================================

bool ComputeShader::ParseAndGenerate()
{
	sh_errorMessage.clear();

	auto& unit = sh_stages[ShaderType::COMPUTE];
	unit.parsed = ShaderParser::ParseShaderFile(unit.path, ShaderType::COMPUTE);
	if (unit.parsed.IsEmpty())
	{
		sh_errorMessage = "Failed to parse compute shader: " + unit.path;
		NEURUS_ERR("[ComputeShader] " << sh_errorMessage);
		return false;
	}
	unit.code = ShaderGenerator::Generate(unit.parsed);

	NEURUS_LOG("[ComputeShader] '" << sh_name << "' parsed and generated");
	return true;
}

void ComputeShader::ReloadContent()
{
	if (sh_compPath.empty()) return;

	sh_stages[ShaderType::COMPUTE].path = sh_compPath;
	ParseAndGenerate();
}

bool ComputeShader::CompileToSpv()
{
	auto& unit = sh_stages[ShaderType::COMPUTE];
	if (!unit.spv.empty()) return true;
	if (unit.code.empty()) return false;

	unit.spv = ShaderLibrary::Compile(unit, ShaderType::COMPUTE, sh_name);
	if (unit.spv.empty())
	{
		NEURUS_ERR("[ComputeShader] CompileToSpv failed for '" << sh_name << "'");
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
	const auto& s = sh_stages.at(ShaderType::COMPUTE).parsed;
	x = s.local_size_x;
	y = s.local_size_y;
	z = s.local_size_z;
}

bool ComputeShader::Recompile()
{
	auto& unit = sh_stages[ShaderType::COMPUTE];
	unit.parsed = ShaderParser::ParseShaderFile(unit.path, ShaderType::COMPUTE);
	if (unit.parsed.IsEmpty())
	{
		sh_errorMessage = "Failed to re-parse: " + unit.path;
		NEURUS_ERR("[ComputeShader] " << sh_errorMessage);
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
	sh_defaults.erase(
		std::remove_if(sh_defaults.begin(), sh_defaults.end(),
			[&](const Default& d) { return d.name == name; }),
		sh_defaults.end());
	sh_defaults.push_back({name, "int", std::to_string(value)});
}

void ComputeShader::SetDefault(const std::string& name, float value)
{
	sh_defaults.erase(
		std::remove_if(sh_defaults.begin(), sh_defaults.end(),
			[&](const Default& d) { return d.name == name; }),
		sh_defaults.end());
	sh_defaults.push_back({name, "float", std::to_string(value)});
}

void ComputeShader::SetDefault(const std::string& name, bool value)
{
	sh_defaults.erase(
		std::remove_if(sh_defaults.begin(), sh_defaults.end(),
			[&](const Default& d) { return d.name == name; }),
		sh_defaults.end());
	sh_defaults.push_back({name, "bool", value ? "true" : "false"});
}

} // namespace neurus
