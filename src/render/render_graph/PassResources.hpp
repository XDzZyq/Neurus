/**
 * @file PassResources.hpp
 * @brief The images one graph node binds for one frame.
 *
 * Resolved by RenderGraph::Execute from the node's connected input sockets and
 * its own output resources. A pass reads them by port name, so it never names
 * an AttachmentName and never looks a resource up by renderer internals.
 */

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace neurus {

class Image;

/**
 * @brief Port-name to image view for one node execution.
 *
 * Non-owning: the images live in RenderCache and outlive the frame.
 */
class PassResources
{
public:
	void SetInput(std::string name, Image* image);
	void SetOutput(std::string name, Image* image);

	/// @brief Input image for a port, or nullptr when the port is unconnected.
	Image* Input(std::string_view port) const;

	/// @brief Output image for a port, or nullptr when the port is not a resource.
	Image* Output(std::string_view port) const;

private:
	std::unordered_map<std::string, Image*> m_inputs;
	std::unordered_map<std::string, Image*> m_outputs;
};

} // namespace neurus
