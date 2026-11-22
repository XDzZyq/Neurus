#include "asset/data/render_graph/RenderGraphRecord.hpp"

#include "asset/data/render_graph/RenderGraphTopology.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/render_graph/PortSource.hpp"
#include "core/IResourceLookup.hpp"

#include <set>
#include <stdexcept>
#include <utility>

namespace neurus::project
{

namespace {

using NodeType  = RenderGraphTopology::NodeType;
using InSocket  = RenderGraphTopology::SocketInT;
using OutSocket = RenderGraphTopology::SocketOutT;

NodeType* FindNode(RenderGraphTopology& graph, const std::string_view name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

OutSocket* FindOutput(NodeType& node, const std::string_view name)
{
	for (const auto& output : node.outputs)
	{
		if (output->name == name)
			return output.get();
	}
	return nullptr;
}

InSocket* FindInput(NodeType& node, const std::string_view name)
{
	for (const auto& input : node.inputs)
	{
		if (input->name == name)
			return input.get();
	}
	return nullptr;
}

CpuPort MakePort(const PassPort& port, std::optional<GraphOutputTag> tag = {})
{
	CpuPort payload;
	payload.name      = port.name;
	payload.direction = port.direction;
	payload.type      = port.type;
	payload.binding   = port.binding;
	payload.location  = port.location;
	payload.tag       = std::move(tag);
	return payload;
}

} // anonymous namespace

RenderGraphRecord ToRecord(const RenderGraphTopology& graph)
{
	RenderGraphRecord record;
	record.nodes.reserve(graph.nodes.size());

	for (const auto& node : graph.nodes)
	{
		RenderGraphNodeRecord saved;
		saved.name      = node->name;
		saved.pass      = node->data.pass;
		saved.shaderUid = node->data.shader ? node->data.shader->GetObjectID() : 0;
		saved.x         = node->data.x;
		saved.y         = node->data.y;

		saved.outputs.reserve(node->outputs.size());
		for (const auto& output : node->outputs)
		{
			RenderGraphPortRecord port;
			port.name     = output->name;
			port.type     = output->data.type;
			port.binding  = output->data.binding;
			port.location = output->data.location;
			port.tag      = output->data.tag;
			saved.outputs.push_back(std::move(port));
		}

		record.nodes.push_back(std::move(saved));
	}

	for (const auto& connection : graph.Connections())
	{
		RenderGraphEdgeRecord edge{connection.from->owner->name, connection.from->name,
		                           connection.to->owner->name, connection.to->name};
		if (connection.to->owner->data.shader)
		{
			edge.inputType = connection.to->data.type;
			edge.inputBinding = connection.to->data.binding;
		}
		record.edges.push_back(std::move(edge));
	}

	record.outputs.emplace();
	for (const auto& [name, endpoint] : graph.sceneOutputs)
		record.outputs->push_back({name, endpoint.node, endpoint.port});
	return record;
}

RenderGraphTopology FromRecord(const RenderGraphRecord& record, const IPortSource& ports,
                               std::vector<std::string>& problems,
                               const IResourceLookup* resources,
                               const std::function<bool(const std::string&, const ShaderData&)>& acceptsShader)
{
	RenderGraphTopology graph;
	graph.nodes.reserve(record.nodes.size());

	std::vector<NodeType*> created;
	std::set<NodeType*> useFallback;
	created.reserve(record.nodes.size());

	for (const RenderGraphNodeRecord& saved : record.nodes)
	{
		CpuNode data;
		data.pass      = saved.pass;
		if (resources && saved.shaderUid != 0)
		{
			auto shader = resources->Get<ShaderData>(saved.shaderUid);
			if (shader && shader->IsValid() && (!acceptsShader || acceptsShader(saved.pass, *shader)))
				data.shader = std::move(shader);
		}
		data.x         = saved.x;
		data.y         = saved.y;

		if (FindNode(graph, saved.name))
			problems.push_back("duplicate node name '" + saved.name + "'");

		NodeType* node = graph.AddNode(saved.name, data);
		created.push_back(node);
		if (saved.shaderUid != 0 && !data.shader)
			useFallback.insert(node);

		for (const RenderGraphPortRecord& savedPort : saved.outputs)
		{
			OutSocket* output = node->AddOutput(savedPort.name);
			output->data.name      = savedPort.name;
			output->data.direction = PortDirection::Output;
			output->data.type      = savedPort.type;
			output->data.binding   = savedPort.binding;
			output->data.location  = savedPort.location;
			output->data.tag       = savedPort.tag;
		}
	}

	for (size_t i = 0; i < created.size(); ++i)
	{
		const RenderGraphNodeRecord& saved = record.nodes[i];
		const PassPorts* passPorts = ports.FindPassPorts(saved.pass);
		if (!passPorts)
		{
			problems.push_back("node '" + saved.name + "' names unknown pass kind '"
			                   + saved.pass + "'");
			continue;
		}

		for (const PassPort& input : EffectivePassPorts(created[i]->data, ports).Inputs())
		{
			InSocket* socket = created[i]->AddInput(input.name);
			socket->data = MakePort(input);
		}
	}

	for (const RenderGraphEdgeRecord& edge : record.edges)
	{
		NodeType* from = FindNode(graph, edge.from);
		NodeType* to   = FindNode(graph, edge.to);
		if (!from || !to)
		{
			problems.push_back("edge '" + edge.from + "." + edge.fromPort + " -> "
			                   + edge.to + "." + edge.toPort
			                   + "' names an unknown node");
			continue;
		}

		OutSocket* output = FindOutput(*from, edge.fromPort);
		if (!output)
		{
			problems.push_back("edge names an unknown output '" + edge.from + "."
			                   + edge.fromPort + "'");
			continue;
		}

		InSocket* input = FindInput(*to, edge.toPort);
		if (useFallback.contains(to) && !edge.inputType.empty())
		{
			const auto* builtin = ports.FindPassPorts(to->data.pass);
			const PassPort role{edge.toPort, PortDirection::Input, edge.inputType, edge.inputBinding, 0};
			const auto* port = builtin ? builtin->FindRole(role) : nullptr;
			input = port ? FindInput(*to, port->name) : nullptr;
		}
		if (!input)
		{
			problems.push_back("edge names an unknown input '" + edge.to + "."
			                   + edge.toPort + "'");
			continue;
		}

		if (input->source)
		{
			problems.push_back("input '" + edge.to + "." + edge.toPort
			                   + "' has more than one producer");
			continue;
		}

		if (!graph.Connect(output, input))
		{
			problems.push_back("connection '" + edge.from + "." + edge.fromPort
			                   + " -> " + edge.to + "." + edge.toPort
			                   + "' was refused (duplicate or cycle)");
		}
	}

	if (record.outputs)
	{
		for (const auto& output : *record.outputs)
			if (!graph.sceneOutputs.emplace(output.name, GraphEndpoint{output.node, output.port}).second)
				problems.push_back("duplicate scene output '" + output.name + "'");
	}
	else
		graph.InferLegacyOutputs();
	for (auto* node : useFallback)
	{
		if (const auto* builtin = ports.FindPassPorts(node->data.pass))
		{
			try
			{
				ReconcilePassPorts(graph, *node, *builtin);
			}
			catch (const std::invalid_argument& error)
			{
				problems.push_back(error.what());
			}
		}
	}
	return graph;
}

} // namespace neurus::project
