/**
 * @file ShaderInterface.hpp
 * @brief CPU validation of compiled vertex/fragment interfaces.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace neurus {

enum class ShaderType;
struct ShaderUnit;
class ShaderStruct;

/// Returns a diagnostic if a shader changes the built-in pass's descriptors, push data or attachment interface.
std::string PassShaderInterfaceProblem(const ShaderUnit& expected, const ShaderUnit& selected, ShaderType stage);

std::string PassShaderInterfaceProblem(const ShaderStruct& expected, std::span<const uint32_t> expectedSpv,
                                       const ShaderStruct& selected, std::span<const uint32_t> selectedSpv,
                                       ShaderType stage);

/** Checks that every user fragment input has a compatible vertex output. */
bool ValidateRenderShaderInterface(std::span<const uint32_t> vertex,
                                  std::span<const uint32_t> fragment,
                                  std::string& error);

} // namespace neurus
