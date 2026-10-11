#include "ShaderCompiler.h"

#include "core/Log.h"

namespace neurus {

ShaderCompiler::ShaderCompiler()
{
	// -- Default compile options --

	// Target Vulkan 1.3 + SPIR-V 1.6
	m_options.SetTargetEnvironment(shaderc_target_env_vulkan,
	                               shaderc_env_version_vulkan_1_3);
	m_options.SetTargetSpirv(shaderc_spirv_version_1_6);

#ifdef NDEBUG
	// Release: optimise for performance
	m_options.SetOptimizationLevel(shaderc_optimization_level_performance);
#else
	// Debug: no optimisation, faster compile times
	m_options.SetOptimizationLevel(shaderc_optimization_level_zero);
#endif
}

std::vector<uint32_t> ShaderCompiler::CompileGlslToSpv(
	const std::string& source,
	shaderc_shader_kind kind,
	const std::string& entryPoint,
	const std::string& inputName)
{
	m_lastError.clear();

	shaderc::SpvCompilationResult result = m_compiler.CompileGlslToSpv(
		source.c_str(),
		source.size(),
		kind,
		inputName.c_str(),
		entryPoint.c_str(),
		m_options);

	if (result.GetCompilationStatus() != shaderc_compilation_status_success)
	{
		m_lastError = result.GetErrorMessage();

		NEURUS_ERR("[ShaderCompiler] " << inputName << ": "
			<< result.GetNumErrors() << " error(s), "
			<< result.GetNumWarnings() << " warning(s)\n"
			<< m_lastError);
		NEURUS_ERR("[ShaderCompiler] GLSL source:\n" << source);

		return {};
	}

	std::vector<uint32_t> spirv;
	spirv.assign(result.cbegin(), result.cend());
	return spirv;
}

void ShaderCompiler::SetOptimizationLevel(shaderc_optimization_level level)
{
	m_options.SetOptimizationLevel(level);
}

void ShaderCompiler::AddMacroDefinition(const std::string& name,
                                        const std::string& value)
{
	m_options.AddMacroDefinition(name, value);
}

void ShaderCompiler::SetGenerateDebugInfo(bool enable)
{
	if (enable)
	{
		m_options.SetGenerateDebugInfo();
	}
	// Note: shaderc has no API to disable debug info once set; callers needing
	// both variants must create separate ShaderCompiler instances.
}

} // namespace neurus
