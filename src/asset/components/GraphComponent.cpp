#include "asset/components/GraphComponent.hpp"

#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"
#include "core/IResourceLookup.hpp"
#include "asset/data/render_graph/PortSource.hpp"
#include "core/Log.hpp"
#include "core/Serialize.hpp"

#include <cereal/cereal.hpp>

#include <exception>
#include <limits>
#include <optional>

#include <nlohmann/json.hpp>
#include <utility>

namespace neurus::project
{

namespace {

using Json = nlohmann::ordered_json;

bool UnsignedWithin(const Json& value, const uint64_t maximum)
{
	if (value.is_number_unsigned())
		return value.get<uint64_t>() <= maximum;
	return value.is_number_integer() && value.get<int64_t>() >= 0
	    && static_cast<uint64_t>(value.get<int64_t>()) <= maximum;
}

bool HasString(const Json& object, const char* key)
{
	const auto value = object.find(key);
	return value != object.end() && value->is_string();
}

bool ValidTag(const Json& tag)
{
	if (!tag.is_object() || !tag.contains("nullopt") || !tag["nullopt"].is_boolean())
		return false;
	if (tag["nullopt"].get<bool>())
		return true;
	if (!tag.contains("data") || !tag["data"].is_object())
		return false;
	const auto& data = tag["data"];
	return data.contains("format") && UnsignedWithin(data["format"], static_cast<uint64_t>(GraphFormat::D32Sfloat))
	    && data.contains("usage") && UnsignedWithin(data["usage"], static_cast<uint64_t>(GraphUsage::DepthSampled))
	    && data.contains("extent") && UnsignedWithin(data["extent"], static_cast<uint64_t>(GraphExtent::Shadow))
	    && data.contains("layers") && UnsignedWithin(data["layers"], static_cast<uint64_t>(GraphLayers::Lights));
}

bool ValidGraphShape(const Json& graph)
{
	if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()
		|| !graph.contains("edges") || !graph["edges"].is_array())
		return false;
	for (const auto& node : graph["nodes"])
	{
		if (!node.is_object() || !HasString(node, "name") || !HasString(node, "pass")
			|| !node.contains("x") || !node["x"].is_number()
			|| !node.contains("y") || !node["y"].is_number()
			|| !node.contains("outputs") || !node["outputs"].is_array())
			return false;
		if (node.contains("shader") && !UnsignedWithin(node["shader"], std::numeric_limits<int>::max()))
			return false;
		for (const auto& port : node["outputs"])
		{
			if (!port.is_object() || !HasString(port, "name") || !HasString(port, "type")
				|| !port.contains("binding") || !UnsignedWithin(port["binding"], std::numeric_limits<uint32_t>::max())
				|| !port.contains("location") || !UnsignedWithin(port["location"], std::numeric_limits<uint32_t>::max())
				|| !port.contains("tag") || !ValidTag(port["tag"]))
				return false;
		}
	}
	for (const auto& edge : graph["edges"])
	{
		if (!edge.is_object() || !HasString(edge, "from") || !HasString(edge, "fromPort")
			|| !HasString(edge, "to") || !HasString(edge, "toPort"))
			return false;
	}
	return true;
}

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

RenderGraphRecord ToRecord(const RenderGraphData& graph)
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
		record.edges.push_back({
			connection.from->owner->name,
			connection.from->name,
			connection.to->owner->name,
			connection.to->name,
		});
	}

	return record;
}

RenderGraphData FromRecord(const RenderGraphRecord& record, const IPortSource& ports,
                           std::vector<std::string>& problems,
                           const IResourceLookup* resources)
{
	RenderGraphData graph;
	graph.nodes.reserve(record.nodes.size());

	std::vector<NodeType*> created;
	created.reserve(record.nodes.size());

	for (const RenderGraphNodeRecord& saved : record.nodes)
	{
		CpuNode data;
		data.pass      = saved.pass;
		if (resources && saved.shaderUid != 0)
		{
			auto shader = resources->Get<ShaderData>(saved.shaderUid);
			if (shader && shader->IsValid())
				data.shader = std::move(shader);
		}
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

		for (const PassPort& input : passPorts->Inputs())
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
                               const std::shared_ptr<RenderGraphData>& graph)
	: m_pending(&pending)
	, m_graph(&graph)
{
}

void GraphComponent::Save(cereal::JSONOutputArchive& ar) const
{
	if (!*m_graph)
		return;
	const RenderGraphRecord record = ToRecord(**m_graph);
	ar(cereal::make_nvp("proj_render_graph", record));
}

void GraphComponent::LoadJson(const std::string& object)
{
	m_pending->reset();
	try
	{
		const Json document = Json::parse(object);
		const auto graph = document.find("proj_render_graph");
		if (graph == document.end() || graph->is_null())
			return;
		if (!ValidGraphShape(*graph))
		{
			NEURUS_LOG("[GraphComponent] malformed graph - keeping null and using runtime defaults");
			return;
		}
		// Guard all scalar/container types before cereal's unchecked Release reads.
		Serializable::LoadJson(object);
	}
	catch (const std::exception& error)
	{
		m_pending->reset();
		NEURUS_LOG("[GraphComponent] cannot load graph: " << error.what() << " - keeping null");
	}
}

void GraphComponent::Load(cereal::JSONInputArchive& ar)
{
	// Every field is optional at the archive level; an absent key means "no
	// saved document", never the previous session's graph.
	m_pending->reset();
	RenderGraphRecord loaded;
	try
	{
		if (!archive::LoadFresh(ar, "proj_render_graph", loaded))
		{
			NEURUS_LOG("GraphComponent::Load: no proj_render_graph node"
			           " - using runtime defaults without storing a project graph.");
			m_pending->reset();
			return;
		}
	}
	catch (const cereal::Exception& e)
	{
		NEURUS_ERR("GraphComponent::Load: " << e.what()
		           << " - using runtime defaults (corrupt graph).");
		m_pending->reset();
		return;
	}

	*m_pending = std::move(loaded);
}

} // namespace neurus::project
