/**
 * @file CpuGraphValidation.hpp
 * @brief Validates the CPU render graph document against the shaders it names.
 *
 * Validation is collected, never thrown: the editor shows every problem and the
 * renderer keeps drawing the previous graph. A problem always names the node and,
 * when it has one, the port.
 */

#pragma once

#include "asset/data/RenderGraphData.hpp"
#include "asset/data/render_graph/PortSource.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace neurus {

class IPassShaderCatalog;

/// Throws if the selected shader cannot satisfy the pass's compiled interface.
void ValidatePassShader(std::string_view passKind, const ShaderData& shader, const IPassShaderCatalog& catalog);

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
std::vector<CpuGraphDiagnostic> ValidateCpuGraph(const RenderGraphData& graph,
                                                 const IPortSource& ports,
                                                 const std::function<CpuPorts(const CpuNode&)>& shaderPorts = {});

/// Checks the document topology and each assigned shader against its pass.
std::vector<CpuGraphDiagnostic> ValidateCpuGraph(const RenderGraphData& graph, const IPassShaderCatalog& catalog);

} // namespace neurus
