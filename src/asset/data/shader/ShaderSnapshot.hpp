/**
 * @file ShaderSnapshot.hpp
 * @brief Owned CPU shader interface and compiled bytecode for renderer publication.
 */

#pragma once

#include "asset/data/render_graph/PassPorts.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace neurus {

class ShaderData;

/// @brief Descriptor kind a binding declares.
enum class DescriptorKind : std::uint8_t
{
	CombinedImageSampler,
	StorageImage,
	UniformBuffer,
	StorageBuffer,
};

/**
 * @brief One reflected descriptor binding.
 *
 * @c stageMask is a bit mask over ShaderType values (1 << type).
 */
struct DescriptorBinding
{
	uint32_t       binding = 0;
	DescriptorKind kind = DescriptorKind::CombinedImageSampler;
	uint32_t       stageMask = 0;
	uint32_t       count = 1;
	bool           partiallyBound = false;

	bool operator==(const DescriptorBinding&) const = default;
};

/// @brief One member of the push-constant block.
struct PushConstantMember
{
	std::string name;
	std::string type;
	uint32_t    offset = 0;
	uint32_t    size = 0;

	bool operator==(const PushConstantMember&) const = default;
};

/**
 * @brief Everything the renderer needs from one pooled shader.
 */
struct ShaderSnapshot
{
	int                             shaderUid = 0;
	int                             version = 0;
	std::vector<PassPort>         ports;
	std::vector<DescriptorBinding>  bindings;
	std::vector<PushConstantMember> pushConstants;
	std::vector<uint32_t>           vertexSpv;
	std::vector<uint32_t>           fragmentSpv;
	std::vector<uint32_t>           computeSpv;
};

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
