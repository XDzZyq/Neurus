#include "asset/components/GraphComponent.hpp"

#include "asset/data/RenderGraphData.hpp"
#include "asset/data/render_graph/PortSource.hpp"
#include "core/Log.hpp"
#include "core/Serialize.hpp"

#include <cereal/cereal.hpp>

#include <optional>
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

CpuPort MakePort(const ShaderPort& port, std::optional<GraphOutputTag> tag = {})
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

RenderGraphRecord ToRecord(const RenderGraphData& graph)
{
	RenderGraphRecord record;
	record.nodes.reserve(graph.nodes.size());

	for (const auto& node : graph.nodes)
	{
		RenderGraphNodeRecord saved;
		saved.name      = node->name;
		saved.pass      = node->data.pass;
		saved.shaderUid = node->data.shaderUid;
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

	for (const auto& node : graph.nodes)
	{
		for (const auto& input : node->inputs)
		{
			if (!input->source)
				continue;
			record.edges.push_back({
				input->source->owner->name,
				input->source->name,
				node->name,
				input->name,
			});
		}
	}

	return record;
}

RenderGraphData FromRecord(const RenderGraphRecord& record, const IPortSource& ports,
                           std::vector<std::string>& problems)
{
	RenderGraphData graph;
	graph.nodes.reserve(record.nodes.size());

	std::vector<NodeType*> created;
	created.reserve(record.nodes.size());

	for (const RenderGraphNodeRecord& saved : record.nodes)
	{
		CpuNode data;
		data.pass      = saved.pass;
		data.shaderUid = saved.shaderUid;
		data.x         = saved.x;
		data.y         = saved.y;

		if (FindNode(graph, saved.name))
			problems.push_back("duplicate node name '" + saved.name + "'");

		NodeType* node = graph.AddNode(saved.name, data);
		created.push_back(node);

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

		for (const ShaderPort& input : passPorts->Inputs())
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

	return graph;
}

GraphComponent::GraphComponent(std::optional<RenderGraphRecord>& pending,
                               const RenderGraphData& graph)
	: m_pending(&pending)
	, m_graph(&graph)
{
}

void GraphComponent::Save(cereal::JSONOutputArchive& ar) const
{
	const RenderGraphRecord record = ToRecord(*m_graph);
	ar(cereal::make_nvp("proj_render_graph", record));
}

void GraphComponent::Load(cereal::JSONInputArchive& ar)
{
	// Every field is optional at the archive level; an absent key means "no
	// saved document", never the previous session's graph.
	RenderGraphRecord loaded;
	try
	{
		if (!archive::LoadFresh(ar, "proj_render_graph", loaded))
		{
			NEURUS_LOG("GraphComponent::Load: no proj_render_graph node"
			           " - the Editor will build the default graph.");
			m_pending->reset();
			return;
		}
	}
	catch (const cereal::Exception& e)
	{
		NEURUS_ERR("GraphComponent::Load: " << e.what()
		           << " - proj_render_graph defaulted (corrupt node).");
		m_pending->reset();
		return;
	}

	*m_pending = std::move(loaded);
}

} // namespace neurus::project
