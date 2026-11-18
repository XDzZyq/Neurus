/**
 * @file PortSource.hpp
 * @brief Resolves a pass kind to the ports its shader declares.
 *
 * The interface is Vulkan-free and survives the flat document: the CPU graph
 * derives its input sockets from the same source, validation checks drift
 * against it, and the renderer's pass factory implements it.
 */

#pragma once

#include "asset/data/render_graph/PassPorts.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace neurus {

/**
 * @brief Resolves a pass kind to the ports and externals its shader declares.
 */
class IPortSource
{
public:
	virtual ~IPortSource() = default;

	/// @brief Returns the pass's ports, or nullptr when the kind is unknown.
	virtual const PassPorts* FindPassPorts(std::string_view passKind) const = 0;

	/// @brief Input ports the pass resolves outside the graph (IBL cubemaps).
	virtual std::vector<std::string> ExternalInputs(std::string_view passKind) const
	{
		return {};
	}
};

} // namespace neurus
