#pragma once

#include <shaderc/shaderc.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace neurus {

/**
 * @brief Internal shaderc wrapper for GLSL-to-SPIR-V compilation.
 *
 * Owns one shaderc::Compiler and a persistent CompileOptions the public API
 * modifies. Internal to src/render/shaders/.
 *
 * @note shaderc::Compiler is expensive to construct (initializes glslang
 *       internally), so keep one instance alive.
 */
class ShaderCompiler
{
public:
	/**
	 * @brief Creates the shaderc compiler and default compile options.
	 *
	 * Targets Vulkan 1.3 / SPIR-V 1.6; optimizes for performance in Release,
	 * zero in Debug.
	 */
	ShaderCompiler();
	~ShaderCompiler() = default;

	// Non-copyable - owns internal glslang state
	ShaderCompiler(const ShaderCompiler&) = delete;
	ShaderCompiler& operator=(const ShaderCompiler&) = delete;

	// Movable
	ShaderCompiler(ShaderCompiler&&) noexcept = default;
	ShaderCompiler& operator=(ShaderCompiler&&) noexcept = default;

	/**
	 * @brief Compiles a GLSL source string to SPIR-V bytecode.
	 *
	 * @param source     GLSL source text (null-terminated or length-delimited).
	 * @param kind       Shader kind (e.g. shaderc_vertex_shader).
	 * @param entryPoint Entry-point function name (default "main").
	 * @param inputName  Diagnostic file name shown in error messages.
	 * @return SPIR-V as a vector of uint32_t words.  Empty vector on failure;
	 *         call GetErrorMessage() for the compiler's error text.
	 */
	std::vector<uint32_t> CompileGlslToSpv(
		const std::string& source,
		shaderc_shader_kind kind,
		const std::string& entryPoint,
		const std::string& inputName);

	/**
	 * @brief Returns the compiler's last error message.
	 *
	 * Cleared at the start of each compilation; populated on failure.
	 */
	const std::string& GetErrorMessage() const { return m_lastError; }

	/** @brief Sets the optimization level for subsequent compilations. */
	void SetOptimizationLevel(shaderc_optimization_level level);

	/**
	 * @brief Adds a preprocessor macro definition (e.g. "USE_SHADOWS" to "1").
	 *
	 * Macros persist across compilations; a repeated name overwrites.
	 *
	 * @param name  Macro name (without -D prefix).
	 * @param value Macro value.
	 */
	void AddMacroDefinition(const std::string& name, const std::string& value);

	/** @brief Controls whether debug info is embedded in the compiled SPIR-V. */
	void SetGenerateDebugInfo(bool enable);

private:
	shaderc::Compiler m_compiler;
	shaderc::CompileOptions m_options;
	std::string m_lastError;
};

} // namespace neurus
