/**
 * @file CpuGraphValidation.cpp
 * @brief Document checks against the port source.
 */

#include "asset/data/render_graph/CpuGraphValidation.hpp"

#include "asset/data/render_graph/PassShaderCatalog.hpp"
#include "asset/data/render_graph/PassKinds.hpp"
#include "asset/data/shader/ShaderInterface.hpp"
#include "asset/data/ShaderData.hpp"

#include <algorithm>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>

namespace neurus {

namespace {

bool Matches(const CpuPort& socket, const PassPort& port)
{
	return socket.name == port.name && socket.direction == port.direction && socket.type == port.type
	    && socket.binding == port.binding && socket.location == port.location;
}

} // anonymous namespace

std::vector<CpuGraphDiagnostic> ValidateCpuGraph(const RenderGraphData& graph,
                                                 const IPortSource& ports,
                                                 const std::function<PassPorts(const CpuNode&)>& shaderPorts)
{
	std::vector<CpuGraphDiagnostic> diagnostics;

	const auto active = graph.ActiveNodes();
	if (!graph.nodes.empty() && !graph.sceneOutputs.contains("color"))
		diagnostics.push_back({{}, "color", "scene color output is not connected"});
	for (const auto& [name, endpoint] : graph.sceneOutputs)
	{
		const CpuPort* producer = nullptr;
		for (const auto& node : graph.nodes)
			if (node->name == endpoint.node)
				for (const auto& out : node->outputs)
					if (out->name == endpoint.port)
						producer = &out->data;
		if (!producer || !producer->tag)
		{
			diagnostics.push_back({endpoint.node, name, "scene output references missing producer '" + endpoint.port + "'"});
			continue;
		}
		const auto& tag = *producer->tag;
		bool compatible = false;
		if (name == "color")
			compatible = tag.format != GraphFormat::R32Uint && tag.format != GraphFormat::None
			          && (tag.usage == GraphUsage::ColorSampled || tag.usage == GraphUsage::StorageSampled);
		else if (name == "depth")
			compatible = tag.format == GraphFormat::D32Sfloat && tag.usage == GraphUsage::DepthSampled;
		else if (name == "object_id")
			compatible = tag.format == GraphFormat::R32Uint && tag.usage == GraphUsage::ColorSampled;
		if (!compatible || tag.extent != GraphExtent::Render || tag.layers != GraphLayers::Single)
			diagnostics.push_back({endpoint.node, name, "incompatible scene output '" + endpoint.port + "'"});
	}

	std::set<std::string> names;
	for (const auto& node : graph.nodes)
	{
		if (!names.insert(node->name).second)
			diagnostics.push_back({node->name, {}, "duplicate node name"});
	}

	for (const auto& node : graph.nodes)
	{
		const PassPorts* builtin = ports.FindPassPorts(node->data.pass);
		if (!builtin)
		{
			diagnostics.push_back(
				{node->name, {}, "unknown pass kind '" + node->data.pass + "'"});
			continue;
		}

		const auto contract = shaderPorts ? shaderPorts(node->data) : EffectivePassPorts(node->data, ports);
		for (const auto& problem : contract.problems)
			diagnostics.push_back({node->name, {}, problem});
		std::set<std::string> outputs;
		for (const auto& output : node->outputs)
		{
			if (!outputs.insert(output->name).second)
				diagnostics.push_back({node->name, output->name, "duplicate output socket"});
			const auto* port = contract.Find(output->name, PortDirection::Output);
			const auto* role = port ? builtin->FindRole(*port) : nullptr;
			if (!role)
				diagnostics.push_back({node->name, output->name, "output port is no longer declared"});
			else if (!Matches(output->data, *port))
				diagnostics.push_back({node->name, output->name, "socket differs from declared output role"});
			if (!output->data.tag)
			{
				diagnostics.push_back({node->name, output->name, "output port has no tag"});
				continue;
			}
			const auto& tag = *output->data.tag;
			const auto expected = role ? BuiltInOutputTag(node->data.pass, role->name) : std::nullopt;
			if (expected && (expected->format != tag.format || expected->usage != tag.usage ||
			    (tag.usage != GraphUsage::Logical && (expected->extent != tag.extent || expected->layers != tag.layers))))
				diagnostics.push_back({node->name, output->name, "image tag is incompatible with the pass attachment"});
		}
		for (const auto& port : contract.Outputs())
			if (!outputs.contains(port.name))
				diagnostics.push_back({node->name, port.name, "declared output port has no socket"});

		const std::vector<std::string> external = ports.ExternalInputs(node->data.pass);
		const std::set<std::string> externalSet(external.begin(), external.end());

		std::set<std::string> inputs;
		for (const auto& input : node->inputs)
		{
			if (!inputs.insert(input->name).second)
				diagnostics.push_back({node->name, input->name, "duplicate input socket"});
			const auto* declared = contract.Find(input->name, PortDirection::Input);
			if (!declared)
				diagnostics.push_back({node->name, input->name, "input port is no longer declared"});
			else if (!Matches(input->data, *declared))
				diagnostics.push_back({node->name, input->name, "socket differs from declared input role"});
			const auto* role = declared ? builtin->FindRole(*declared) : nullptr;
			const bool externalInput = role && externalSet.contains(role->name);
			if (input->IsConnected() && externalInput)
				diagnostics.push_back({node->name, input->name, "external input cannot be connected"});
			if (input->IsConnected() && input->incoming.from->data.tag)
			{
				const auto& source = *input->incoming.from->data.tag;
				const bool shadow = node->data.pass == "ShadowIntensityPass" && role
				                 && (role->name == "u_ShadowCube" || role->name == "u_SunShadowMap");
				const bool array = input->data.type.find("Array") != std::string::npos;
				const bool uintInput = input->data.type.starts_with("usampler");
				if (shadow ? source.usage != GraphUsage::Logical
				           : (source.usage == GraphUsage::Logical || array != (source.layers == GraphLayers::Lights)
				              || uintInput != (source.format == GraphFormat::R32Uint)))
					diagnostics.push_back({node->name, input->name, "incompatible producer image type"});
			}
			if (active.contains(node->name) && !input->IsConnected() && !externalInput)
			{
				diagnostics.push_back({node->name, input->name,
				                       "input port is not connected and is not external"});
			}
		}

		for (const auto& port : contract.Inputs())
			if (!inputs.contains(port.name))
				diagnostics.push_back({node->name, port.name, "declared input port has no socket"});
	}

	const std::vector<RenderGraphData::NodeType*> cyclic = graph.CyclicNodes();
	if (!cyclic.empty())
	{
		std::string names;
		for (const RenderGraphData::NodeType* node : cyclic)
		{
			if (!names.empty())
				names += ", ";
			names += node->name;
		}
		diagnostics.push_back({{}, {}, "cycle detected involving: " + names});
	}

	return diagnostics;
}

void ValidatePassShader(std::string_view passKind, const ShaderData& shader, const IPassShaderCatalog& catalog)
{
	const auto* kind = FindPassKind(passKind);
	if (!kind || kind->shaders.empty())
		throw std::invalid_argument("Unknown pass shader recipe: " + std::string(passKind));
	const auto expected = catalog.ShaderFor(kind->shaders.front().name);
	if (!expected || shader.GetType() != expected->GetType() || !shader.IsValid())
		throw std::invalid_argument(std::string(passKind) + ": incompatible or uncompiled shader stages");
	for (const auto stage : {ShaderType::VERTEX, ShaderType::FRAGMENT, ShaderType::COMPUTE})
	{
		const auto* ref = expected->FindStage(stage);
		if (!ref)
			continue;
		const auto& selected = shader.GetStage(stage);
		const auto problem = PassShaderInterfaceProblem(*ref, selected, stage);
		if (!problem.empty())
			throw std::invalid_argument(std::string(passKind) + " " + ShaderData::TypeToString(stage) + ": " + problem);
	}
	if (shader.GetType() == ShaderType::VERTEX)
	{
		std::string error;
		if (!ValidateRenderShaderInterface(shader.GetStage(ShaderType::VERTEX).spv,
		                                  shader.GetStage(ShaderType::FRAGMENT).spv, error))
			throw std::invalid_argument(std::string(passKind) + ": " + error);
	}
}

std::vector<CpuGraphDiagnostic> ValidateCpuGraph(const RenderGraphData& graph, const IPassShaderCatalog& catalog)
{
	auto diagnostics = ValidateCpuGraph(graph, catalog.Ports());
	for (const auto& node : graph.nodes)
	{
		if (!node->data.shader)
			continue;
		try
		{
			ValidatePassShader(node->data.pass, *node->data.shader, catalog);
		}
		catch (const std::exception& error)
		{
			diagnostics.push_back({node->name, {}, error.what()});
		}
	}
	return diagnostics;
}

} // namespace neurus
