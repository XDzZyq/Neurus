/**
 * @file PassBindings.hpp
 * @brief Builds and writes the descriptor bindings a graph node owns.
 *
 * The graph builds its plan from runtime sockets. Standalone pass tests
 * build a plan from the shader ports. Both use the same descriptor rules.
 */

#pragma once

#include "../passes/Pass.hpp"
#include "asset/data/render_graph/CPUPorts.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace neurus {

/**
 * @brief One image descriptor in a node's compiled binding plan.
 */
struct GraphImageBinding
{
	std::string       port;
	PortDirection     direction = PortDirection::Input;
	uint32_t          binding = 0;
	vk::DescriptorType type = vk::DescriptorType::eCombinedImageSampler;
	vk::ImageLayout   layout = vk::ImageLayout::eShaderReadOnlyOptimal;
};

/// Appends the graph-managed image descriptor for a single port, when applicable.
void AppendImageBinding(std::vector<GraphImageBinding>& bindings, std::string_view name,
                        PortDirection direction, std::string_view type, uint32_t binding,
                        std::span<const uint32_t> selfManaged);

/**
 * @brief Builds the image bindings for one standalone pass.
 *
 * Inputs are included when @p connected says so (an empty predicate includes
 * every sampler input, which is what a direct pass test wants). Outputs are
 * included for every storage-image port. Self-managed bindings are skipped.
 *
 * @param ports      Ports the pass declares.
 * @param pass       Pass whose SelfManagedBindings() applies.
 * @param connected  Optional input-port predicate; empty means all connected.
 */
std::vector<GraphImageBinding> BuildImageBindings(
	const CPUPorts& ports,
	const Pass& pass,
	const std::function<bool(std::string_view)>& connected = {});

/**
 * @brief Writes image, buffer and external-image descriptors into the pass's set.
 *
 * @param pass       Pass whose set is written.
 * @param cache      Resource cache for buffer/external-image collection.
 * @param ctx        Per-frame context (frame index).
 * @param resources  Resolved input/output images for this node.
 * @param bindings   Compiled image bindings.
 */
void WritePassBindings(Pass& pass,
                       RenderCache& cache,
                       const RenderContext& ctx,
                       const PassResources& resources,
                       std::span<const GraphImageBinding> bindings);

} // namespace neurus
