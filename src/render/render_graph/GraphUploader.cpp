/**
 * @file GraphUploader.cpp
 * @brief CPU document to runtime graph, through the generic morphism.
 */

#include "render/render_graph/GraphUploader.hpp"

#include <stdexcept>
#include <string>

namespace neurus {

namespace {

using GpuNode   = RenderGraph::NodeT;
using GpuOutput = RenderGraph::SocketOutT;
using GpuInput  = RenderGraph::SocketInT;

GpuOutput* FindOutput(GpuNode& node, const std::string_view name)
{
	for (const auto& output : node.outputs)
	{
		if (output->name == name)
			return output.get();
	}
	return nullptr;
}

GpuInput* FindInput(GpuNode& node, const std::string_view name)
{
	for (const auto& input : node.inputs)
	{
		if (input->name == name)
			return input.get();
	}
	return nullptr;
}

} // anonymous namespace

UploadResult UploadCpuGraph(const RenderGraphData& cpu, PassFactory& factory,
                            const std::span<const ShaderSnapshot> shaders)
{
	UploadResult result;

	const auto findSnapshot = [shaders](const int shaderUid) -> const ShaderSnapshot*
	{
		if (shaderUid == 0)
			return nullptr;
		for (const ShaderSnapshot& snapshot : shaders)
		{
			if (snapshot.shaderUid == shaderUid)
				return &snapshot;
		}
		return nullptr;
	};

	for (const auto& node : cpu.nodes)
	{
		if (node->data.shaderUid != 0 && !findSnapshot(node->data.shaderUid))
		{
			result.errors.push_back(
				{node->name, {}, "no shader snapshot for uid "
				                 + std::to_string(node->data.shaderUid)});
		}
	}

	if (!result.errors.empty())
		return result;

	// The renderer validates the document against the same ports it will wire:
	// a missing tag, a stale socket or an unconnected input keeps the previous
	// graph drawing instead of failing later inside a recorded frame.
	const std::vector<CpuGraphDiagnostic> diagnostics = ValidateCpuGraph(cpu, factory);
	if (!diagnostics.empty())
	{
		result.errors = diagnostics;
		return result;
	}

	// The morphism's policy creates the target objects. A null factory result
	// leaves the node unmapped, and Validate() names it.
	RenderGraphMorphism::Policy policy;
	policy.createNode = [&factory](RenderGraph::GraphT& target,
	                               const RenderGraphData::NodeType& node) -> GpuNode*
	{
		Pass* pass = factory.Find(node.data.pass);
		const PassPorts* ports = factory.FindPassPorts(node.data.pass);
		if (!pass || !ports)
			return nullptr;

		GpuNode* gpu = RenderGraph::MakePassNode(
			target, pass, *ports, factory.ExternalInputs(node.data.pass), node.name);
		if (gpu)
		{
			// The runtime node keeps the document id so diagnostics and the
			// resource keys name the graph the user edited.
			gpu->name = node.name;
		}
		return gpu;
	};

	policy.createOutput = [](GpuNode& node, const RenderGraphData::SocketOutT& port)
		-> GpuOutput*
	{
		return FindOutput(node, port.name);
	};

	policy.createInput = [](GpuNode& node, const RenderGraphData::SocketInT& port)
		-> GpuInput*
	{
		return FindInput(node, port.name);
	};

	RenderGraphMorphism morphism(cpu, std::move(policy));
	RenderGraph::GraphT target = morphism.Map(cpu);
	result.morphism = std::move(morphism);

	for (const std::string& problem : result.morphism.Validate().problems)
		result.errors.push_back({{}, {}, problem});

	if (!result.errors.empty())
		return result;

	auto graph = std::make_unique<RenderGraph>();
	graph->Adopt(std::move(target));

	try
	{
		graph->Compile();
	}
	catch (const std::exception& error)
	{
		result.errors.push_back({{}, {}, error.what()});
		return result;
	}

	result.graph = std::move(graph);
	return result;
}

} // namespace neurus
