/**
 * @file GraphUploader.hpp
 * @brief Builds the runtime graph from the Editor's CPU document.
 *
 * The uploader is the one place a document crosses into the renderer. Every
 * node becomes a pass instance from the factory, every socket is materialized
 * from that pass's ports, and every CPU connection becomes a runtime edge. The
 * map between the two graphs is a `GraphMorphism`, so a node or edge the
 * renderer could not reproduce is reported instead of silently dropped.
 *
 * The document's `shaderUid`s are checked against the request's snapshots: a
 * node whose shader has no snapshot is rejected, so the renderer never draws a
 * node against a shader the Editor did not send. Installing a snapshot's SPIR-V
 * into a pass instance is the pass factory's job and lands with the shader
 * editing surface; until then the factory's own pinned shaders are used.
 */

#pragma once

#include "asset/data/render_graph/RenderGraphData.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "render/RenderFrameSnapshot.hpp"
#include "core/GraphMorphism.hpp"
#include "PassFactory.hpp"
#include "RenderGraph.hpp"

#include <memory>
#include <span>
#include <vector>

namespace neurus {

/// @brief Functor from the CPU document to the runtime graph.
using RenderGraphMorphism = GraphMorphism<RenderGraphData, RenderGraph::GraphT>;

/**
 * @brief Outcome of one upload.
 *
 * @c graph is null when any diagnostic was produced; the caller keeps drawing
 * the previous graph.
 *
 * @note @c morphism records images into the request's document and into
 *       @c graph, so it may only be queried while both are alive. The renderer
 *       reads its verdict and discards it.
 */
struct UploadResult
{
	std::unique_ptr<RenderGraph>    graph;
	RenderGraphMorphism             morphism;
	std::vector<CpuGraphDiagnostic> errors;
};

/**
 * @brief Instantiates and wires the document against the pass factory.
 *
 * @param cpu      Document to upload; it is not modified.
 * @param factory  Pass instances and their ports.
 * @param shaders  Snapshots the Editor sent with the document.
 * @return The runtime graph plus its morphism, or null plus the diagnostics.
 */
UploadResult UploadCpuGraph(const RenderGraphData& cpu,
                            PassFactory& factory,
                            std::span<const ShaderSnapshot> shaders);

} // namespace neurus
