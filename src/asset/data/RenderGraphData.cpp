/**
 * @file RenderGraphData.cpp
 * @brief Default document and hash.
 */

#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"

#include "asset/data/render_graph/PassKinds.hpp"
#include "asset/data/render_graph/PassShaderCatalog.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neurus {

namespace {

using NodeType  = RenderGraphData::NodeType;
using InSocket  = RenderGraphData::SocketInT;
using OutSocket = RenderGraphData::SocketOutT;

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

std::optional<GraphOutputTag> BuiltInOutputTag(std::string_view kind, std::string_view port)
{
	struct Output
	{
		std::string_view kind;
		std::string_view port;
		GraphOutputTag tag;
	};
	static constexpr Output tags[] = {
		{"GeometryPass", "gPosition", {GraphFormat::Rgba16f,    GraphUsage::ColorSampled}},
		{"GeometryPass", "gNormal", {GraphFormat::Rgba16f,    GraphUsage::ColorSampled}},
		{"GeometryPass", "gAlbedo", {GraphFormat::Rgba8Srgb,  GraphUsage::ColorSampled}},
		{"GeometryPass", "gMetallicRoughness", {GraphFormat::Rgba8Unorm, GraphUsage::ColorSampled}},
		{"GeometryPass", "gObjectID", {GraphFormat::R32Uint,    GraphUsage::ColorSampled}},
		{"GeometryPass", "depth", {GraphFormat::D32Sfloat,  GraphUsage::DepthSampled}},
		{"ShadowDepthPass", "shadow_depth", {GraphFormat::None, GraphUsage::Logical}},
		{"ShadowIntensityPass", "outputShadow", {GraphFormat::R8Unorm, GraphUsage::StorageSampled,
		                  GraphExtent::Render, GraphLayers::Lights}},
		{"SSAOPass", "outputSSAO", {GraphFormat::R8Unorm, GraphUsage::StorageSampled}},
		{"LightingPass", "outputImage", {GraphFormat::Rgba16f, GraphUsage::StorageSampled}},
		{"ComposePass", "outputComposed", {GraphFormat::Rgba16f, GraphUsage::StorageSampled}},
		{"FXAAPass", "outputFXAA", {GraphFormat::Rgba16f, GraphUsage::StorageSampled}},
	};
	for (const auto& output : tags)
		if (output.kind == kind && output.port == port)
			return output.tag;
	return {};
}

RenderGraphData::RenderGraphData(const IPortSource& ports)
{
	const auto addNode = [this, &ports](
		const std::string& name, const std::string& pass) -> NodeType*
	{
		const PassPorts* passPorts = ports.FindPassPorts(pass);
		if (!passPorts)
		{
			throw std::runtime_error(
				"No ports for built-in pass '" + pass + "'");
		}

		CpuNode data;
		data.pass = pass;
		NodeType* node = AddNode(name, data);

		for (const PassPort& input : passPorts->Inputs())
			node->AddInput(input.name)->data = MakePort(input);

		for (const PassPort& output : passPorts->Outputs())
			node->AddOutput(output.name)->data = MakePort(output, BuiltInOutputTag(pass, output.name));

		return node;
	};

	const auto connect = [this](NodeType& from, const std::string& fromPort,
	                            NodeType& to, const std::string& toPort)
	{
		OutSocket* output = FindOutput(from, fromPort);
		InSocket* input   = FindInput(to, toPort);
		if (!output || !input || !Connect(output, input))
		{
			throw std::runtime_error(
				"Invalid default graph edge '" + from.name + "."
				+ fromPort + " -> " + to.name + "." + toPort + "'");
		}
	};

	NodeType* geometry = addNode("geometry", "GeometryPass");
	NodeType* shadowDepth = addNode("shadow_depth", "ShadowDepthPass");
	NodeType* shadowIntensity = addNode("shadow_intensity", "ShadowIntensityPass");
	NodeType* ssao = addNode("ssao", "SSAOPass");
	NodeType* lighting = addNode("lighting", "LightingPass");
	NodeType* compose = addNode("compose", "ComposePass");
	NodeType* fxaa = addNode("fxaa", "FXAAPass");

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

	InferLegacyOutputs();
}

std::uint64_t RenderGraphData::Hash() const
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
	hashString(std::to_string(nodes.size()));
	for (const auto& node : nodes)
	{
		hashByte(0x02);
		hashString(node->name);
		hashString(node->data.pass);
		if (const auto& shader = node->data.shader)
		{
			hashByte(0x08);
			hashString(std::to_string(shader->GetObjectID()));
		}
		else
		{
			hashByte(0x09);
		}
		const auto hashPort = [&](const CpuPort& port)
		{
			hashString(port.name);
			hashString(port.type);
			hashByte(static_cast<unsigned char>(port.direction));
			hashString(std::to_string(port.binding));
			hashString(std::to_string(port.location));
		};
		hashString(std::to_string(node->inputs.size()));
		for (const auto& input : node->inputs)
		{
			hashString(input->name);
			hashPort(input->data);
		}
		hashString(std::to_string(node->outputs.size()));
		for (const auto& output : node->outputs)
		{
			hashByte(0x03);
			hashString(output->name);
			hashPort(output->data);
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
	for (const auto& connection : Connections())
	{
		hashByte(0x07);
		hashString(connection.from->owner->name);
		hashString(connection.from->name);
		hashString(connection.to->owner->name);
		hashString(connection.to->name);
	}

	for (const auto& [name, endpoint] : sceneOutputs)
	{
		hashString(name);
		hashString(endpoint.node);
		hashString(endpoint.port);
	}
	return hash;
}

std::unique_ptr<RenderGraphData> RenderGraphData::Clone() const
{
	auto copy = std::make_unique<RenderGraphData>();
	copy->nodes = CloneGraph<GraphT>(*this).nodes;
	copy->sceneOutputs = sceneOutputs;
	copy->m_pending = m_pending;
	return copy;
}

void RenderGraphData::SwapContents(RenderGraphData& other) noexcept
{
	nodes.swap(other.nodes);
	sceneOutputs.swap(other.sceneOutputs);
	m_pending.swap(other.m_pending);
}

void RenderGraphData::InferLegacyOutputs()
{
	sceneOutputs.clear();
	for (const auto& node : nodes)
	{
		if (node->data.pass == "FXAAPass")
			sceneOutputs["color"] = {node->name, "outputFXAA"};
		else if (node->data.pass == "ComposePass")
			sceneOutputs.try_emplace("color", GraphEndpoint{node->name, "outputComposed"});
		else if (node->data.pass == "GeometryPass")
		{
			sceneOutputs["depth"] = {node->name, "depth"};
			sceneOutputs["object_id"] = {node->name, "gObjectID"};
		}
	}
}

std::set<std::string> RenderGraphData::ActiveNodes() const
{
	std::vector<const NodeType*> pending;
	for (const auto& [name, endpoint] : sceneOutputs)
	{
		for (const auto& node : nodes)
			if (node->name == endpoint.node)
				pending.push_back(node.get());
	}
	std::set<std::string> active;
	while (!pending.empty())
	{
		const auto* node = pending.back();
		pending.pop_back();
		if (!active.insert(node->name).second)
			continue;
		for (const auto& input : node->inputs)
			if (input->source)
				pending.push_back(input->source->owner);
	}
	return active;
}

PassPorts WithShaderPortNames(const PassPorts& builtin, const PassPorts& shaderPorts)
{
	PassPorts result;
	result.kind = builtin.kind;
	result.problems = builtin.problems;
	result.problems.insert(result.problems.end(), shaderPorts.problems.begin(), shaderPorts.problems.end());
	for (const auto& port : shaderPorts.ports)
	{
		if (!builtin.FindRole(port))
			result.problems.push_back("unsupported shader port '" + port.name + "'");
	}
	for (auto role : builtin.ports)
	{
		if (const auto* port = shaderPorts.FindRole(role))
			role.name = port->name;
		result.Add(role);
	}
	return result;
}

PassPorts EffectivePassPorts(const CpuNode& node, const IPortSource& catalog)
{
	const auto* builtin = catalog.FindPassPorts(node.pass);
	if (!builtin)
		throw std::invalid_argument("unknown pass kind '" + node.pass + "'");
	if (!node.shader)
		return *builtin;
	const ShaderData* shaders[] = {node.shader.get()};
	return WithShaderPortNames(*builtin, ExtractPassPorts(node.pass, shaders));
}

void ReconcilePassPorts(RenderGraphData& graph, RenderGraphData::NodeType& node, const PassPorts& ports)
{
	if (!ports.problems.empty())
		throw std::invalid_argument(node.name + ": " + ports.problems.front());
	const auto roleFor = [&](const auto& socket) {
		const auto& data = socket->data;
		const PassPort role{data.name, data.direction, data.type, data.binding, data.location};
		return ports.FindRole(role);
	};
	const auto validate = [&](const auto& sockets) {
		for (const auto& socket : sockets)
			if (!roleFor(socket))
				throw std::invalid_argument(node.name + "." + socket->name + ": incompatible shader socket");
	};
	validate(node.inputs);
	validate(node.outputs);

	const auto oldOutputs = graph.sceneOutputs;
	const auto reconcile = [&](auto& sockets)
	{
		for (auto& socket : sockets)
		{
			const auto* port = roleFor(socket);
			if (port->direction == PortDirection::Output)
			{
				for (auto& [name, endpoint] : graph.sceneOutputs)
					if (endpoint.node == node.name && oldOutputs.at(name).port == socket->name)
						endpoint.port = port->name;
			}
			socket->name = socket->data.name = port->name;
		}
	};
	reconcile(node.inputs);
	reconcile(node.outputs);
}

RenderGraphData::RenderGraphData(project::RenderGraphRecord record)
	: m_pending(std::move(record))
{
}

bool RenderGraphData::Resolve(const IPassShaderCatalog& catalog, const IResourceLookup& resources,
                              std::vector<std::string>& diagnostics)
{
	if (!m_pending)
		return true;

	const auto acceptsShader = [&](const std::string& kind, const ShaderData& shader)
	{
		try
		{
			catalog.ValidatePassShader(kind, shader);
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
	auto topology = project::FromRecord(*m_pending, ports, errors, &resources, acceptsShader);
	if (errors.empty())
	{
		for (const auto& diagnostic : ValidateCpuGraph(*topology, ports))
			errors.push_back(diagnostic.node + "." + diagnostic.port + ": " + diagnostic.message);
	}
	if (!errors.empty())
	{
		diagnostics.insert(diagnostics.end(), errors.begin(), errors.end());
		return false;
	}

	// The resolved document has no pending record. The old record leaves with the temporary.
	SwapContents(*topology);
	return true;
}

} // namespace neurus
