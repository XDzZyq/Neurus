/**
 * @file RenderGraphCompile.cpp
 * @brief Validation and topological planning for RenderGraphSpec.
 */

#include "asset/graph/RenderGraphCompile.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace neurus {

namespace {

void AddDiagnostic(CompileResult& result,
                   std::string node,
                   std::string port,
                   std::string message)
{
	result.diagnostics.push_back(
		{std::move(node), std::move(port), std::move(message)});
}

/**
 * @brief Returns the reason a tag is invalid, or an empty string when it is valid.
 */
std::string TagProblem(const GraphOutputTag& tag)
{
	if (tag.usage == GraphUsage::Logical)
	{
		if (tag.format != GraphFormat::None)
			return "logical output tag must not name a format";
		return {};
	}

	if (tag.format == GraphFormat::None)
		return "output tag has no format";

	return {};
}

struct ResolvedEdge
{
	size_t from = 0;
	size_t to   = 0;
};

} // anonymous namespace

CompileResult CompileRenderGraphSpec(const RenderGraphSpec& spec,
                                     const IPortSource& ports,
                                     const bool fxaa)
{
	CompileResult result;
	result.spec = PruneConditionalNodes(spec, fxaa);

	const size_t nodeCount = result.spec.nodes.size();

	std::unordered_map<std::string, size_t> nodeIndex;
	nodeIndex.reserve(nodeCount);

	std::vector<const PassPorts*> nodePorts(nodeCount, nullptr);

	// --- Nodes: unique ids, known pass kinds, pass-side problems ---
	for (size_t i = 0; i < nodeCount; ++i)
	{
		const GraphNodeSpec& node = result.spec.nodes[i];

		const auto [it, inserted] = nodeIndex.emplace(node.id, i);
		(void)it;
		if (!inserted)
		{
			AddDiagnostic(result, node.id, {}, "duplicate node id");
			continue;
		}

		const PassPorts* passPorts = ports.FindPassPorts(node.pass);
		if (!passPorts)
		{
			AddDiagnostic(result, node.id, {},
			              "unknown pass kind '" + node.pass + "'");
			continue;
		}

		nodePorts[i] = passPorts;
		for (const std::string& problem : passPorts->problems)
			AddDiagnostic(result, node.id, {}, problem);
	}

	// --- Edges: endpoints, directions, duplicates, producers ---
	std::set<std::tuple<std::string, std::string, std::string, std::string>> seenEdges;
	std::map<std::pair<std::string, std::string>,
	         std::pair<std::string, std::string>> producers;
	std::set<std::pair<std::string, std::string>> connectedInputs;
	std::vector<ResolvedEdge> resolvedEdges;

	for (const GraphEdgeSpec& edge : result.spec.edges)
	{
		const auto fromIt = nodeIndex.find(edge.from);
		const auto toIt   = nodeIndex.find(edge.to);

		if (fromIt == nodeIndex.end())
		{
			AddDiagnostic(result, edge.from, edge.fromPort,
			              "edge names an unknown node");
		}
		if (toIt == nodeIndex.end())
		{
			AddDiagnostic(result, edge.to, edge.toPort,
			              "edge names an unknown node");
		}
		if (fromIt == nodeIndex.end() || toIt == nodeIndex.end())
			continue;

		const auto key = std::make_tuple(edge.from, edge.fromPort, edge.to, edge.toPort);
		if (!seenEdges.insert(key).second)
		{
			AddDiagnostic(result, edge.to, edge.toPort,
			              "duplicate edge from '" + edge.from + "." + edge.fromPort + "'");
			continue;
		}

		resolvedEdges.push_back({fromIt->second, toIt->second});

		const PassPorts* fromPorts = nodePorts[fromIt->second];
		const PassPorts* toPorts   = nodePorts[toIt->second];

		const ShaderPort* fromPort =
			fromPorts ? fromPorts->Find(edge.fromPort, PortDirection::Output) : nullptr;
		if (!fromPort)
		{
			const bool isInput =
				fromPorts && fromPorts->Find(edge.fromPort, PortDirection::Input);
			AddDiagnostic(result, edge.from, edge.fromPort,
			              isInput ? "port is an input, not an output"
			                      : "output port is not declared");
		}

		const ShaderPort* toPort =
			toPorts ? toPorts->Find(edge.toPort, PortDirection::Input) : nullptr;
		if (!toPort)
		{
			const bool isOutput =
				toPorts && toPorts->Find(edge.toPort, PortDirection::Output);
			AddDiagnostic(result, edge.to, edge.toPort,
			              isOutput ? "port is an output, not an input"
			                       : "input port is not declared");
		}

		connectedInputs.emplace(edge.to, edge.toPort);

		const auto producerKey = std::make_pair(edge.to, edge.toPort);
		const auto [producerIt, producerInserted] = producers.emplace(
			producerKey, std::make_pair(edge.from, edge.fromPort));
		if (!producerInserted)
		{
			AddDiagnostic(result, edge.to, edge.toPort,
			              "input has more than one producer ('"
			              + producerIt->second.first + "." + producerIt->second.second
			              + "' and '" + edge.from + "." + edge.fromPort + "')");
		}
	}

	// --- Tags: every tag names a declared output, every used output has a tag ---
	for (size_t i = 0; i < nodeCount; ++i)
	{
		const PassPorts* passPorts = nodePorts[i];
		if (!passPorts)
			continue;

		const GraphNodeSpec& node = result.spec.nodes[i];
		for (const auto& [portName, tag] : node.outputs)
		{
			if (!passPorts->Find(portName, PortDirection::Output))
			{
				AddDiagnostic(result, node.id, portName,
				              "tag names an unknown output port");
				continue;
			}

			const std::string problem = TagProblem(tag);
			if (!problem.empty())
				AddDiagnostic(result, node.id, portName, problem);
		}
	}

	for (const GraphEdgeSpec& edge : result.spec.edges)
	{
		const auto fromIt = nodeIndex.find(edge.from);
		if (fromIt == nodeIndex.end())
			continue;

		const PassPorts* passPorts = nodePorts[fromIt->second];
		if (!passPorts || !passPorts->Find(edge.fromPort, PortDirection::Output))
			continue;

		const GraphNodeSpec& node = result.spec.nodes[fromIt->second];
		if (!node.outputs.contains(edge.fromPort))
		{
			AddDiagnostic(result, edge.from, edge.fromPort,
			              "output port has no tag");
		}
	}

	// --- Every live input is connected or declared external ---
	for (size_t i = 0; i < nodeCount; ++i)
	{
		const PassPorts* passPorts = nodePorts[i];
		if (!passPorts)
			continue;

		const GraphNodeSpec& node = result.spec.nodes[i];
		const std::vector<std::string> external = ports.ExternalInputs(node.pass);
		const std::set<std::string> externalSet(external.begin(), external.end());

		for (const ShaderPort& input : passPorts->Inputs())
		{
			const auto key = std::make_pair(node.id, input.name);
			if (connectedInputs.contains(key) || externalSet.contains(input.name))
				continue;

			AddDiagnostic(result, node.id, input.name,
			              "input port is not connected and is not external");
		}
	}

	// --- Kahn: a cycle leaves nodes with a non-zero in-degree ---
	std::vector<std::vector<size_t>> successors(nodeCount);
	std::vector<size_t> inDegree(nodeCount, 0);
	for (const ResolvedEdge& edge : resolvedEdges)
	{
		successors[edge.from].push_back(edge.to);
		++inDegree[edge.to];
	}

	std::vector<size_t> stack;
	stack.reserve(nodeCount);
	for (size_t i = 0; i < nodeCount; ++i)
	{
		if (inDegree[i] == 0)
			stack.push_back(i);
	}

	std::vector<size_t> order;
	order.reserve(nodeCount);
	std::vector<size_t> remaining = inDegree;

	while (!stack.empty())
	{
		const size_t index = stack.back();
		stack.pop_back();
		order.push_back(index);

		for (const size_t successor : successors[index])
		{
			if (--remaining[successor] == 0)
				stack.push_back(successor);
		}
	}

	if (order.size() != nodeCount)
	{
		std::string cyclic;
		for (size_t i = 0; i < nodeCount; ++i)
		{
			if (remaining[i] == 0)
				continue;
			if (!cyclic.empty())
				cyclic += ", ";
			cyclic += result.spec.nodes[i].id;
		}
		AddDiagnostic(result, {}, {}, "cycle detected involving: " + cyclic);
	}

	result.ok = result.diagnostics.empty();
	if (result.ok)
		result.order = std::move(order);

	return result;
}

} // namespace neurus
