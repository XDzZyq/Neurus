/**
 * @file CpuGraphValidation.cpp
 * @brief Document checks against the port source.
 */

#include "asset/data/render_graph/CpuGraphValidation.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace neurus {

namespace {

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

const PassPort* FindDeclared(const PassPorts& ports, const std::string& name,
                               const PortDirection direction)
{
	return ports.Find(name, direction);
}

} // anonymous namespace

std::vector<CpuGraphDiagnostic> ValidateCpuGraph(const RenderGraphTopology& graph,
                                                 const IPortSource& ports)
{
	std::vector<CpuGraphDiagnostic> diagnostics;

	std::set<std::string> names;
	for (const auto& node : graph.nodes)
	{
		if (!names.insert(node->name).second)
			diagnostics.push_back({node->name, {}, "duplicate node name"});
	}

	for (const auto& node : graph.nodes)
	{
		const PassPorts* passPorts = ports.FindPassPorts(node->data.pass);
		if (!passPorts)
		{
			diagnostics.push_back(
				{node->name, {}, "unknown pass kind '" + node->data.pass + "'"});
			continue;
		}

		for (const auto& output : node->outputs)
		{
			if (!output->data.tag)
			{
				diagnostics.push_back({node->name, output->name,
				                       "output port has no tag"});
			}
			else
			{
				const std::string problem = TagProblem(*output->data.tag);
				if (!problem.empty())
					diagnostics.push_back({node->name, output->name, problem});
			}

			if (!FindDeclared(*passPorts, output->name, PortDirection::Output))
			{
				diagnostics.push_back({node->name, output->name,
				                       "output port is no longer declared"});
			}
		}

		const std::vector<std::string> external = ports.ExternalInputs(node->data.pass);
		const std::set<std::string> externalSet(external.begin(), external.end());

		for (const auto& input : node->inputs)
		{
			if (!FindDeclared(*passPorts, input->name, PortDirection::Input))
			{
				diagnostics.push_back({node->name, input->name,
				                       "input port is no longer declared"});
			}

			if (!input->IsConnected() && !externalSet.contains(input->name))
			{
				diagnostics.push_back({node->name, input->name,
				                       "input port is not connected and is not external"});
			}
		}

		for (const PassPort& declared : passPorts->Inputs())
		{
			const bool present = std::any_of(
				node->inputs.begin(), node->inputs.end(),
				[&declared](const auto& socket) { return socket->name == declared.name; });
			if (!present)
			{
				diagnostics.push_back({node->name, declared.name,
				                       "declared input port has no socket"});
			}
		}
	}

	const std::vector<RenderGraphTopology::NodeType*> cyclic = graph.CyclicNodes();
	if (!cyclic.empty())
	{
		std::string names;
		for (const RenderGraphTopology::NodeType* node : cyclic)
		{
			if (!names.empty())
				names += ", ";
			names += node->name;
		}
		diagnostics.push_back({{}, {}, "cycle detected involving: " + names});
	}

	return diagnostics;
}

} // namespace neurus
