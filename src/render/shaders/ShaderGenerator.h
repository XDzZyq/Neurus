/**
 * @file ShaderGenerator.h
 * @brief Generates valid Vulkan GLSL strings from ShaderStruct IR.
 *
 * Parse-then-generate pipeline:
 *   ShaderParser  → ShaderStruct (IR)
 *   ShaderGenerator → valid Vulkan GLSL string
 *
 * Separated from ShaderStruct to keep the IR pure data - the generator
 * reads the public fields and emits GLSL without mutating the struct
 * (except clearing is_struct_changed on completion).
 */

#pragma once

#include <string>

namespace neurus {

// Forward declarations
class ShaderStruct;
enum class Interp : uint8_t;

/**
 * @brief Pure function: ShaderStruct IR -> Vulkan GLSL.
 */
class ShaderGenerator
{
public:
	/**
	 * @brief Generate a complete Vulkan GLSL string from the given IR.
	 * @param shaderStruct Populated ShaderStruct (from ShaderParser).
	 * @return Valid GLSL source ready for shaderc/spirv compilation.
	 * @note Clears shaderStruct.is_struct_changed on return.
	 */
	static std::string Generate(ShaderStruct& shaderStruct);

	ShaderGenerator() = delete;

private:
	/// Emit interpolation qualifier: "flat ", "noperspective ", or empty.
	static const char* InterpStr(Interp i);
};

} // namespace neurus
