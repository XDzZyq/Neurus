/**
 * @file GraphUploader.cpp
 * @brief CPU document to runtime graph, through the generic morphism.
 */

#include "render/render_graph/GraphUploader.hpp"

#include <stdexcept>
#include <string>
#include <unordered_map>

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

UploadResult UploadCpuGraph(const CpuRenderGraph& cpu, PassFactory& factory,
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

	auto graph = std::make_unique<RenderGraph>();
	std::unordered_map<const CpuNode*, GpuNode*> nodeImages;

	// Instantiate every node first: the morphism's target set is fixed when it
	// is constructed, so the nodes must exist before MapGraph maps them.
	for (const auto& node : cpu.nodes)
	{
		Pass* pass = factory.Find(node->data.pass);
		const PassPorts* ports = factory.FindPassPorts(node->data.pass);
		if (!pass || !ports)
		{
			result.errors.push_back({node->name, {}, "no pass instance or ports for kind '"
			                                        + node->data.pass + "'"});
			continue;
		}

		GpuNode* gpu = graph->AddPass(pass, *ports,
		                              factory.ExternalInputs(node->data.pass),
		                              node->name);
		if (!gpu)
		{
			result.errors.push_back({node->name, {}, "the runtime graph rejected the node"});
			continue;
		}

		// The runtime node keeps the document id so diagnostics and the
		// resource keys name the graph the user edited.
		gpu->name = node->name;
		nodeImages.emplace(&node->data, gpu);
	}

	if (!result.errors.empty())
		return result;

	GraphMapPolicy<CpuRenderGraph, RenderGraph::GraphT> policy;
	policy.mapNode = [&nodeImages](const CpuRenderGraph::NodeType& node) -> GpuNode*
	{
		const auto it = nodeImages.find(&node.data);
		return it == nodeImages.end() ? nullptr : it->second;
	};

	policy.mapOutput = [&nodeImages](const CpuRenderGraph::NodeType& node,
	                                 const CpuRenderGraph::SocketOutT& port) -> GpuOutput*
	{
		const auto it = nodeImages.find(&node.data);
		if (it == nodeImages.end() || !it->second)
			return nullptr;
		return FindOutput(*it->second, port.name);
	};

	policy.mapInput = [&nodeImages](const CpuRenderGraph::NodeType& node,
	                                const CpuRenderGraph::SocketInT& port) -> GpuInput*
	{
		const auto it = nodeImages.find(&node.data);
		if (it == nodeImages.end() || !it->second)
			return nullptr;
		return FindInput(*it->second, port.name);
	};

	result.morphism = MapGraph(cpu, graph->GraphData(), policy);

	for (const std::string& problem : result.morphism.Validate().problems)
		result.errors.push_back({{}, {}, problem});

	if (!result.errors.empty())
		return result;

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
