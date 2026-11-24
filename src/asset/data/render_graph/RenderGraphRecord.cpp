#include "asset/data/render_graph/RenderGraphRecord.hpp"

#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/render_graph/PortSource.hpp"
#include "asset/data/render_graph/PassShaderCatalog.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "core/Log.hpp"
#include "core/IResourceLookup.hpp"

#include <set>
#include <stdexcept>
#include <utility>

namespace neurus::project
{

namespace {

using NodeType  = RenderGraphData::NodeType;
using InSocket  = RenderGraphData::SocketInT;
using OutSocket = RenderGraphData::SocketOutT;

NodeType* FindNode(RenderGraphData& graph, const std::string_view name)
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

RenderGraphNodeRecord ToRecord(const NodeType& node)
{
	RenderGraphNodeRecord saved;
	saved.name      = node.name;
	saved.pass      = node.data.pass;
	saved.shader    = node.data.shader.IdentityOnly();
	saved.x         = node.data.x;
	saved.y         = node.data.y;

	saved.outputs.reserve(node.outputs.size());
	for (const auto& output : node.outputs)
	{
		RenderGraphPortRecord port;
		port.name     = output->name;
		port.type     = output->data.type;
		port.binding  = output->data.binding;
		port.location = output->data.location;
		port.tag      = output->data.tag;
		saved.outputs.push_back(std::move(port));
	}
	return saved;
}

RenderGraphRecord ToRecord(const RenderGraphData& graph)
{
	RenderGraphRecord record;
	record.nodes.reserve(graph.nodes.size());

	for (const auto& node : graph.nodes)
		record.nodes.push_back(ToRecord(*node));

	for (const auto* connection : graph.Connections())
	{
		RenderGraphEdgeRecord edge{connection->from->owner->name, connection->from->name,
		                           connection->to->owner->name, connection->to->name};
		if (connection->to->owner->data.shader)
		{
			edge.inputType = connection->to->data.type;
			edge.inputBinding = connection->to->data.binding;
		}
		record.edges.push_back(std::move(edge));
	}

	for (const auto& [name, endpoint] : graph.sceneOutputs)
		record.outputs.push_back({name, endpoint.node, endpoint.port});
	return record;
}

namespace
{
std::unique_ptr<NodeType> RestoreNode(
	const RenderGraphNodeRecord& saved, const IPortSource& ports, std::vector<std::string>& problems,
	const std::function<bool(const std::string&, const ShaderData&)>& acceptsShader)
{
	auto node = std::make_unique<NodeType>();
	node->name = saved.name;
	node->data.pass = saved.pass;
	node->data.x = saved.x;
	node->data.y = saved.y;
	if (saved.shader && saved.shader->IsValid() && (!acceptsShader || acceptsShader(saved.pass, *saved.shader)))
		node->data.shader = saved.shader;

	for (const auto& port : saved.outputs)
		node->AddOutput(port.name)->data = {port.name, PortDirection::Output, port.type,
		                                    port.binding, port.location, port.tag};
	if (!ports.FindPassPorts(saved.pass))
		problems.push_back("node '" + saved.name + "' names unknown pass kind '" + saved.pass + "'");
	else
		for (const auto& input : EffectivePassPorts(node->data, ports).Inputs())
			node->AddInput(input.name)->data = MakePort(input);
	return node;
}

struct GraphReadServices final : ResourceReadServices
{
	std::shared_ptr<const IPassShaderCatalog> catalog;
	std::weak_ptr<std::vector<std::string>> diagnostics;
};

void RestoreTopology(RenderGraphData& graph, const RenderGraphRecord& record, const IPortSource& ports, std::vector<std::string>& problems,
	const std::function<bool(const std::string&, const ShaderData&)>& acceptsShader)
{
	graph.nodes.reserve(record.nodes.size());

	std::set<NodeType*> useFallback;
	for (const auto& saved : record.nodes)
	{
		if (FindNode(graph, saved.name))
			problems.push_back("duplicate node name '" + saved.name + "'");
		auto* node = graph.AddNode(RestoreNode(saved, ports, problems, acceptsShader));
		if (saved.shader.id() && !node->data.shader)
			useFallback.insert(node);
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

		if (input->IsConnected())
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

	for (const auto& output : record.outputs)
		if (!graph.sceneOutputs.emplace(output.name, GraphEndpoint{output.node, output.port}).second)
			problems.push_back("duplicate scene output '" + output.name + "'");
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
}
} // namespace

// Portable records (undo, scripting) rebind against the pool at replay.
std::unique_ptr<NodeType> FromRecord(
	const RenderGraphNodeRecord& saved, const IPortSource& ports, std::vector<std::string>& problems,
	const IResourceLookup* resources, const std::function<bool(const std::string&, const ShaderData&)>& acceptsShader)
{
	auto record = saved;
	if (resources) record.shader.Resolve(*resources);
	return RestoreNode(record, ports, problems, acceptsShader);
}

std::unique_ptr<RenderGraphData> FromRecord(
	const RenderGraphRecord& record, const IPortSource& ports, std::vector<std::string>& problems)
{
	auto graph = std::make_unique<RenderGraphData>();
	RestoreTopology(*graph, record, ports, problems, {});
	return graph;
}

std::shared_ptr<const ResourceReadServices> MakeGraphReadServices(
	const IPassShaderCatalog& catalog, std::weak_ptr<std::vector<std::string>> diagnostics)
{
	auto services = std::make_shared<GraphReadServices>();
	services->catalog = catalog.Share();
	services->diagnostics = std::move(diagnostics);
	return services;
}

bool RestoreGraph(RenderGraphData& graph, const RenderGraphRecord& record,
                  const ResourceReadServices* services)
{
	const auto* graphServices = dynamic_cast<const GraphReadServices*>(services);
	if (!graphServices)
	{
		NEURUS_ERR("Missing pass catalog for graph UID " << graph.GetObjectID());
		return false;
	}
	const auto& catalog = *graphServices->catalog;
	std::vector<std::string> diagnostics;
	const auto acceptsShader = [&](const std::string& kind, const ShaderData& shader)
	{
		try
		{
			ValidatePassShader(kind, shader, catalog);
			return true;
		}
		catch (const std::exception& error)
		{
			diagnostics.push_back(kind + " shader UID " + std::to_string(shader.GetObjectID())
			                      + " uses its built-in fallback: " + error.what());
			return false;
		}
	};

	const auto& ports = catalog.Ports();
	std::vector<std::string> errors;
	RestoreTopology(graph, record, ports, errors, acceptsShader);
	if (errors.empty())
		for (const auto& diagnostic : ValidateCpuGraph(graph, ports))
			errors.push_back(diagnostic.node + "." + diagnostic.port + ": " + diagnostic.message);
	diagnostics.insert(diagnostics.end(), errors.begin(), errors.end());
	const auto sink = graphServices->diagnostics.lock();
	for (const auto& message : diagnostics)
	{
		const auto text = "Graph UID " + std::to_string(graph.GetObjectID()) + ": " + message;
		NEURUS_LOG("[RenderGraphData] " << text);
		if (sink) sink->push_back(text);
	}
	return errors.empty();
}

} // namespace neurus::project
