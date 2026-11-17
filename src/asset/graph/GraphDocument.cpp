/**
 * @file GraphDocument.cpp
 * @brief Record conversion and the built-in default document.
 */

#include "asset/graph/GraphDocument.hpp"

#include "asset/graph/PassKinds.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neurus {

namespace {

using NodeType  = CpuRenderGraph::NodeType;
using InSocket  = CpuRenderGraph::SocketInT;
using OutSocket = CpuRenderGraph::SocketOutT;

NodeType* FindNode(CpuRenderGraph& graph, const std::string_view name)
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

CpuGraphRecord ToRecord(const CpuRenderGraph& graph)
{
	CpuGraphRecord record;
	record.nodes.reserve(graph.nodes.size());

	for (const auto& node : graph.nodes)
	{
		CpuNodeRecord saved;
		saved.name      = node->name;
		saved.pass      = node->data.pass;
		saved.shaderUid = node->data.shaderUid;
		saved.x         = node->data.x;
		saved.y         = node->data.y;

		saved.outputs.reserve(node->outputs.size());
		for (const auto& output : node->outputs)
		{
			CpuPortRecord port;
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

CpuRenderGraph FromRecord(const CpuGraphRecord& record, const IPortSource& ports,
                          std::vector<std::string>& problems)
{
	CpuRenderGraph graph;
	graph.nodes.reserve(record.nodes.size());

	std::vector<NodeType*> created;
	created.reserve(record.nodes.size());

	for (const CpuNodeRecord& saved : record.nodes)
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

		for (const CpuPortRecord& savedPort : saved.outputs)
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
		const CpuNodeRecord& saved = record.nodes[i];
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

	for (const CpuEdgeRecord& edge : record.edges)
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

CpuRenderGraph MakeDefaultCpuGraph(const IPortSource& ports)
{
	CpuRenderGraph graph;

	const auto addNode = [&graph, &ports](
		const std::string& name, const std::string& pass,
		const std::vector<std::pair<std::string, GraphOutputTag>>& tags) -> NodeType*
	{
		const PassPorts* passPorts = ports.FindPassPorts(pass);
		if (!passPorts)
		{
			throw std::runtime_error(
				"MakeDefaultCpuGraph: no ports for built-in pass '" + pass + "'");
		}

		CpuNode data;
		data.pass = pass;
		NodeType* node = graph.AddNode(name, data);

		for (const ShaderPort& input : passPorts->Inputs())
			node->AddInput(input.name)->data = MakePort(input);

		for (const ShaderPort& output : passPorts->Outputs())
		{
			std::optional<GraphOutputTag> tag;
			for (const auto& [portName, declared] : tags)
			{
				if (portName == output.name)
				{
					tag = declared;
					break;
				}
			}
			node->AddOutput(output.name)->data = MakePort(output, tag);
		}

		return node;
	};

	const auto connect = [&graph](NodeType& from, const std::string& fromPort,
	                              NodeType& to, const std::string& toPort)
	{
		OutSocket* output = FindOutput(from, fromPort);
		InSocket* input   = FindInput(to, toPort);
		if (!output || !input || !graph.Connect(output, input))
		{
			throw std::runtime_error(
				"MakeDefaultCpuGraph: bad default edge '" + from.name + "." + fromPort
				+ " -> " + to.name + "." + toPort + "'");
		}
	};

	NodeType* geometry = addNode("geometry", "GeometryPass", {
		{"gPosition",         {GraphFormat::Rgba16f,    GraphUsage::ColorSampled}},
		{"gNormal",           {GraphFormat::Rgba16f,    GraphUsage::ColorSampled}},
		{"gAlbedo",           {GraphFormat::Rgba8Srgb,  GraphUsage::ColorSampled}},
		{"gMetallicRoughness", {GraphFormat::Rgba8Unorm, GraphUsage::ColorSampled}},
		{"gObjectID",         {GraphFormat::R32Uint,    GraphUsage::ColorSampled}},
		{"depth",             {GraphFormat::D32Sfloat,  GraphUsage::DepthSampled}},
	});

	NodeType* shadowDepth = addNode("shadow_depth", "ShadowDepthPass", {
		// Per-light cubemaps and sun maps are owned by LightGPU; the graph
		// carries the dependency as a logical token.
		{"shadow_depth", {GraphFormat::None, GraphUsage::Logical}},
	});

	NodeType* shadowIntensity = addNode("shadow_intensity", "ShadowIntensityPass", {
		{"outputShadow", {GraphFormat::R8Unorm, GraphUsage::StorageSampled,
		                  GraphExtent::Render, GraphLayers::Lights}},
	});

	NodeType* ssao = addNode("ssao", "SSAOPass", {
		{"outputSSAO", {GraphFormat::R8Unorm, GraphUsage::StorageSampled}},
	});

	NodeType* lighting = addNode("lighting", "LightingPass", {
		{"outputImage", {GraphFormat::Rgba16f, GraphUsage::StorageSampled}},
	});

	NodeType* compose = addNode("compose", "ComposePass", {
		{"outputComposed", {GraphFormat::Rgba16f, GraphUsage::StorageSampled}},
	});

	NodeType* fxaa = addNode("fxaa", "FXAAPass", {
		{"outputFXAA", {GraphFormat::Rgba16f, GraphUsage::StorageSampled}},
	});

	// Geometry fans out to SSAO, shadow intensity and lighting.
	connect(*geometry, "gPosition", *ssao, "gPosition");
	connect(*geometry, "gNormal", *ssao, "gNormal");
	connect(*geometry, "gAlbedo", *ssao, "gAlpha");
	connect(*geometry, "gPosition", *shadowIntensity, "gPosition");

	// The shadow bundle feeds both the cubemap and the sun-map variants.
	connect(*shadowDepth, "shadow_depth", *shadowIntensity, "u_ShadowCube");
	connect(*shadowDepth, "shadow_depth", *shadowIntensity, "u_SunShadowMap");

	connect(*geometry, "gPosition", *lighting, "gPosition");
	connect(*geometry, "gNormal", *lighting, "gNormal");
	connect(*geometry, "gAlbedo", *lighting, "gAlbedo");
	connect(*geometry, "gMetallicRoughness", *lighting, "gMetallicRoughness");
	connect(*ssao, "outputSSAO", *lighting, "U_AO");
	connect(*shadowIntensity, "outputShadow", *lighting, "U_ShadowArray");

	connect(*lighting, "outputImage", *compose, "gHDRColor");
	connect(*compose, "outputComposed", *fxaa, "gInput");

	return graph;
}

void ApplyConditions(CpuRenderGraph& graph, const bool fxaa)
{
	std::vector<NodeType*> dropped;
	for (const auto& node : graph.nodes)
	{
		const PassKindInfo* kind = FindPassKind(node->data.pass);
		if (!kind || kind->condition.empty())
			continue;

		const bool enabled = (kind->condition == "fxaa") ? fxaa : true;
		if (!enabled)
			dropped.push_back(node.get());
	}

	for (NodeType* node : dropped)
		graph.RemoveNode(node);
}

std::uint64_t CpuGraphHash(const CpuRenderGraph& graph)
{
	constexpr std::uint64_t kOffset = 14695981039346656037ull;
	constexpr std::uint64_t kPrime  = 1099511628211ull;

	std::uint64_t hash = kOffset;

	const auto hashString = [&hash](const std::string_view text)
	{
		for (const char ch : text)
		{
			hash ^= static_cast<unsigned char>(ch);
			hash *= kPrime;
		}
		// A separator keeps "ab" + "c" distinct from "a" + "bc".
		hash ^= 0xFFu;
		hash *= kPrime;
	};

	const auto hashByte = [&hash](const unsigned char value)
	{
		hash ^= value;
		hash *= kPrime;
	};

	hashByte(0x01);
	hashString(std::to_string(graph.nodes.size()));
	for (const auto& node : graph.nodes)
	{
		hashByte(0x02);
		hashString(node->name);
		hashString(node->data.pass);
		hashString(std::to_string(node->data.shaderUid));
		hashString(std::to_string(node->outputs.size()));
		for (const auto& output : node->outputs)
		{
			hashByte(0x03);
			hashString(output->name);
			hashString(output->data.type);
			if (output->data.tag)
			{
				hashByte(0x04);
				hashByte(static_cast<unsigned char>(output->data.tag->format));
				hashByte(static_cast<unsigned char>(output->data.tag->usage));
				hashByte(static_cast<unsigned char>(output->data.tag->extent));
				hashByte(static_cast<unsigned char>(output->data.tag->layers));
			}
			else
			{
				hashByte(0x05);
			}
		}
	}

	hashByte(0x06);
	for (const auto& node : graph.nodes)
	{
		for (const auto& input : node->inputs)
		{
			if (!input->source)
				continue;
			hashByte(0x07);
			hashString(input->source->owner->name);
			hashString(input->source->name);
			hashString(node->name);
			hashString(input->name);
		}
	}

	return hash;
}

} // namespace neurus
