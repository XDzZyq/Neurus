/**
 * @file RenderGraphData.cpp
 * @brief Default document and hash.
 */

#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"

#include "asset/data/render_graph/PassKinds.hpp"
#include "asset/data/render_graph/PortSource.hpp"

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

RenderGraphData RenderGraphData::Default(const IPortSource& ports)
{
	RenderGraphData graph;

	const auto addNode = [&graph, &ports](
		const std::string& name, const std::string& pass,
		const std::vector<std::pair<std::string, GraphOutputTag>>& tags) -> NodeType*
	{
		const PassPorts* passPorts = ports.FindPassPorts(pass);
		if (!passPorts)
		{
			throw std::runtime_error(
				"RenderGraphData::Default: no ports for built-in pass '" + pass + "'");
		}

		CpuNode data;
		data.pass = pass;
		NodeType* node = graph.AddNode(name, data);

		for (const PassPort& input : passPorts->Inputs())
			node->AddInput(input.name)->data = MakePort(input);

		for (const PassPort& output : passPorts->Outputs())
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
				"RenderGraphData::Default: bad default edge '" + from.name + "."
				+ fromPort + " -> " + to.name + "." + toPort + "'");
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
	for (const auto& connection : Connections())
	{
		hashByte(0x07);
		hashString(connection.from->owner->name);
		hashString(connection.from->name);
		hashString(connection.to->owner->name);
		hashString(connection.to->name);
	}

	return hash;
}

} // namespace neurus
