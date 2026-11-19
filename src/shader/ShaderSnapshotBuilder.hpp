/**
 * @file ShaderSnapshotBuilder.hpp
 * @brief Reflects shader data into an owned CPU snapshot.
 */

#pragma once

#include "asset/data/shader/ShaderSnapshot.hpp"

#include <span>

namespace neurus {

class ShaderData;

/**
 * @brief Flattens a pooled shader into a snapshot.
 *
 * @param shader            Shader bundle to read.
 * @param optionalBindings  Bindings the pass marks PARTIALLY_BOUND.
 * @throws std::runtime_error when two stages declare one binding with
 *         conflicting types or counts.
 */
ShaderSnapshot MakeShaderSnapshot(const ShaderData& shader,
                                  std::span<const uint32_t> optionalBindings = {});

} // namespace neurus
