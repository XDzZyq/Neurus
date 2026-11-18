/**
 * @file PassPorts.hpp
 * @brief Derives a pass's image ports from its parsed shader IR.
 *
 * Stage 1 does not reflect SPIR-V: ShaderStruct already carries the fragment
 * pass_list and the sampler/image uniform declarations, so those are the ports.
 * Vertex attributes and inter-stage varyings are never ports.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace neurus {

class ShaderData;

/**
 * @brief Direction of one image port in the graph.
 */
enum class PortDirection
{
	Input,
	Output,
};

/**
 * @brief One named image the graph can carry.
 *
 * A fragment output has a @c location and binding 0; a compute image or sampler
 * has a @c binding and location 0. @c type is the GLSL type string, so the
 * renderer can choose a descriptor type without re-reading the shader.
 */
struct PassPort
{
	std::string   name;
	PortDirection direction = PortDirection::Input;
	std::string   type;          ///< "sampler2D", "image2DArray", "vec4", "uint"
	uint32_t      binding = 0;   ///< set-0 binding; 0 for a fragment output
	uint32_t      location = 0;  ///< fragment output location; 0 otherwise

	bool operator==(const PassPort&) const = default;
};

/**
 * @brief The ports of one pass, merged over every stage of every shader it owns.
 */
struct PassPorts
{
	std::string             kind;
	std::vector<PassPort> ports;
	std::vector<std::string> problems;  ///< conflicting duplicate declarations

	/**
	 * @brief Finds a port by name and direction.
	 * @return Non-owning pointer, or nullptr when absent.
	 */
	const PassPort* Find(std::string_view name, PortDirection direction) const;

	/// @brief Every input port, in declaration order.
	std::vector<PassPort> Inputs() const;

	/// @brief Every output port, in declaration order.
	std::vector<PassPort> Outputs() const;
};

/// @brief True for a GLSL type that binds as a combined image sampler.
bool IsSamplerPortType(std::string_view type);

/// @brief True for a GLSL type that binds as a storage image.
bool IsStorageImagePortType(std::string_view type);

/**
 * @brief Extracts ports from every stage of every shader a pass owns.
 *
 * Graphics: a fragment stage's pass_list entries are outputs; sampler uniforms
 * in any stage are inputs. Compute: sampler uniforms are inputs and image
 * uniforms are outputs. UBOs, SSBOs, plain uniforms and non-image arrays are not
 * ports. @p declaredOutputs carries outputs a pass writes without a shader
 * binding, such as the G-Buffer depth or the per-light shadow bundle.
 *
 * A duplicate (direction, name) pair with a differing type, binding or location
 * is recorded in @c problems rather than silently choosing one declaration.
 *
 * @param kind             Pass kind, stored for diagnostics.
 * @param shaders          Shaders owned by the pass; null entries are skipped.
 * @param declaredOutputs  Extra outputs that have no shader declaration.
 */
PassPorts ExtractPassPorts(std::string kind,
                           std::span<const ShaderData* const> shaders,
                           std::span<const PassPort> declaredOutputs = {});

} // namespace neurus
