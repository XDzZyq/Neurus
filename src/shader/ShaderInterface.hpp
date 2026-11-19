/**
 * @file ShaderInterface.hpp
 * @brief CPU validation of compiled vertex/fragment interfaces.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace neurus {

/** Checks that every user fragment input has a compatible vertex output. */
bool ValidateRenderShaderInterface(std::span<const uint32_t> vertex,
                                  std::span<const uint32_t> fragment,
                                  std::string& error);

} // namespace neurus
