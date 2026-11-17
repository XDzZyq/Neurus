/**
 * @file RenderGraphCompile.hpp
 * @brief Validates a RenderGraphSpec against the ports its passes declare.
 *
 * The compiler is Vulkan-free and has no renderer types: it produces a pruned,
 * validated document plus a topological order, and the renderer maps that order
 * onto concrete pass instances. Every failure is a named diagnostic; the
 * renderer keeps its previous graph when compilation fails.
 */

#pragma once

#include "asset/graph/PassPorts.hpp"
#include "asset/graph/PortSource.hpp"
#include "asset/data/render_graph/RenderGraphSpec.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace neurus {

/**
 * @brief One named compile failure.
 */
struct GraphDiagnostic
{
	std::string node;
	std::string port;
	std::string message;
};

/**
 * @brief Result of one compile.
 *
 * @c spec and @c order are only meaningful when @c ok is true.
 */
struct CompileResult
{
	bool                         ok = false;
	RenderGraphSpec              spec;
	std::vector<size_t>          order;       ///< topological node indices into spec.nodes
	std::vector<GraphDiagnostic> diagnostics;
};

/**
 * @brief Prunes conditional nodes, validates the document and linearizes it.
 *
 * Checks, in order: duplicate node ids, unknown pass kinds and port problems,
 * edge endpoints and directions, duplicate edges, a second producer for one
 * input, output-tag completeness and values, unconnected inputs that are not
 * declared external, and cycles. Diagnostics are collected rather than thrown.
 *
 * @param spec  Document to compile; the input is not modified.
 * @param ports Pass-kind port source.
 * @param fxaa  Whether the hard-coded FXAA condition is on.
 */
CompileResult CompileRenderGraphSpec(const RenderGraphSpec& spec,
                                     const IPortSource& ports,
                                     bool fxaa);

} // namespace neurus
