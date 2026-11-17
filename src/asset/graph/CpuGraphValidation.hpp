/**
 * @file CpuGraphValidation.hpp
 * @brief Validates the CPU render graph document against the shaders it names.
 *
 * Validation is collected, never thrown: the editor shows every problem and the
 * renderer keeps drawing the previous graph. A problem always names the node and,
 * when it has one, the port.
 */

#pragma once

#include "asset/data/render_graph/CpuRenderGraph.hpp"
#include "asset/graph/PortSource.hpp"

#include <string>
#include <vector>

namespace neurus {

/**
 * @brief One named problem in the document.
 */
struct CpuGraphDiagnostic
{
	std::string node;
	std::string port;
	std::string message;

	bool operator==(const CpuGraphDiagnostic&) const = default;
};

/**
 * @brief Collects every problem in @p graph.
 *
 * Checks duplicate node names, unknown pass kinds, missing or inconsistent output
 * tags, sockets the pass no longer declares, unconnected inputs that are not
 * declared external, and cycles.
 *
 * @param graph  Document to check; it is not modified.
 * @param ports  Pass-kind port source.
 * @return One entry per problem; empty when the document is valid.
 */
std::vector<CpuGraphDiagnostic> ValidateCpuGraph(const CpuRenderGraph& graph,
                                                 const IPortSource& ports);

} // namespace neurus
