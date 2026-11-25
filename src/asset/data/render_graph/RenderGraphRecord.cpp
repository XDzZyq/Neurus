#include "asset/data/render_graph/RenderGraphRecord.hpp"

#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/render_graph/PassKinds.hpp"
#include "core/Log.hpp"
#include "core/IResourceLookup.hpp"

#include <algorithm>
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

} // anonymous namespace

RenderGraphNodeRecord ToRecord(const NodeType& node)
{
	RenderGraphNodeRecord saved;
	saved.name      = node.name;
	saved.pass      = node.data.pass;
	saved.shaderUid = node.data.shader.identity();
	saved.x         = node.data.x;
	saved.y         = node.data.y;

	saved.inputs.reserve(node.inputs.size());
	for (const auto& input : node.inputs)
	{
		auto& port = saved.inputs.emplace_back(input->data);
		port.name = input->name;
		port.direction = PortDirection::Input;
	}
	saved.outputs.reserve(node.outputs.size());
	for (const auto& output : node.outputs)
	{
		auto& port = saved.outputs.emplace_back(output->data);
		port.name = output->name;
		port.direction = PortDirection::Output;
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
		record.edges.push_back({connection->from->owner->name, connection->from->name,
		                        connection->to->owner->name, connection->to->name});

	for (const auto& [name, endpoint] : graph.sceneOutputs)
		record.outputs.push_back({name, endpoint.node, endpoint.port});
	return record;
}

namespace
{
std::unique_ptr<NodeType> RestoreNode(const RenderGraphNodeRecord& saved, const IResourceLookup* pool)
{
	auto node = std::make_unique<NodeType>();
	node->name = saved.name;
	node->data.pass = saved.pass;
	node->data.x = saved.x;
	node->data.y = saved.y;
	if (pool && saved.shaderUid)
	{
		auto shader = pool->Get<const ShaderData>(*saved.shaderUid);
		if (shader && shader->IsValid())
			node->data.shader = std::move(shader);
	}

	for (const auto& port : saved.inputs)
		node->AddInput(port.name)->data = port;
	for (const auto& port : saved.outputs)
		node->AddOutput(port.name)->data = port;
	return node;
}

void RestoreTopology(RenderGraphData& graph, const RenderGraphRecord& record,
                     std::vector<std::string>& problems, const IResourceLookup* pool)
{
	graph.nodes.reserve(record.nodes.size());
	for (const auto& saved : record.nodes)
	{
		if (FindNode(graph, saved.name))
			problems.push_back("duplicate node name '" + saved.name + "'");
		graph.AddNode(RestoreNode(saved, pool));
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
}

void DiagnoseShader(const RenderGraphNodeRecord& saved, const NodeType& node, int graphUid)
{
	if (!saved.shaderUid)
		return;

	const auto report = [&](const std::string& message) {
		NEURUS_ERR("[RenderGraphData] Graph UID " << graphUid << ", node '" << node.name
		           << "', shader UID " << *saved.shaderUid << ": " << message);
	};
	if (!node.data.shader)
	{
		report("missing or invalid shader");
		return;
	}

	const ShaderData* shaders[] = {node.data.shader.get()};
	auto declared = ExtractPassPorts(saved.pass, shaders);
	if (const auto* kind = FindPassKind(saved.pass))
		AddDeclaredPassOutputs(declared, *kind);
	for (const auto& problem : declared.problems)
		report(problem);
	const auto check = [&](const std::vector<CpuPort>& sockets, const std::vector<CpuPort>& shaderPorts) {
		for (const auto& socket : sockets)
		{
			const auto* port = declared.Find(socket.name, socket.direction);
			if (!port || port->type != socket.type || port->binding != socket.binding
				|| port->location != socket.location)
				report("saved socket '" + socket.name + "' differs from the shader declaration");
		}
		for (const auto& port : shaderPorts)
			if (std::ranges::none_of(sockets, [&](const CpuPort& socket) { return socket.name == port.name; }))
				report("shader port '" + port.name + "' has no saved socket");
	};
	check(saved.inputs, declared.Inputs());
	check(saved.outputs, declared.Outputs());
}

} // namespace

std::unique_ptr<NodeType> FromRecord(const RenderGraphNodeRecord& saved, const IResourceLookup& pool)
{
	return RestoreNode(saved, &pool);
}

std::unique_ptr<RenderGraphData> FromRecord(
	const RenderGraphRecord& record, std::vector<std::string>& problems, const IResourceLookup* pool)
{
	auto graph = std::make_unique<RenderGraphData>();
	RestoreTopology(*graph, record, problems, pool);
	return graph;
}

bool RestoreGraph(RenderGraphData& graph, const RenderGraphRecord& record, const IResourceLookup& pool)
{
	std::vector<std::string> problems;
	RestoreTopology(graph, record, problems, &pool);
	const int graphUid = graph.GetObjectID();
	for (const auto& problem : problems)
		NEURUS_ERR("[RenderGraphData] Graph UID " << graphUid << ": " << problem);
	if (!problems.empty())
		return false;

	for (size_t i = 0; i < graph.nodes.size(); ++i)
		DiagnoseShader(record.nodes[i], *graph.nodes[i], graphUid);
	return true;
}

} // namespace neurus::project
