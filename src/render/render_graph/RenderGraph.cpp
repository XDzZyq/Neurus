/**
 * @file RenderGraph.cpp
 * @brief Implementation of RenderGraph.
 */

#include "RenderGraph.hpp"
#include "resources/ShaderGPU.hpp"

#include "../DescriptorManager.hpp"
#include "../Image.hpp"
#include "render_api/RenderContext.hpp"
#include "ResourceTag.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

namespace neurus {

namespace {

/// @brief Finds an output socket by port name.
RenderGraph::SocketOutT* FindOutput(RenderGraph::NodeT* node, const std::string_view name)
{
	for (const auto& output : node->outputs)
	{
		if (output->name == name)
			return output.get();
	}
	return nullptr;
}

/// @brief Finds an input socket by port name.
RenderGraph::SocketInT* FindInput(RenderGraph::NodeT* node, const std::string_view name)
{
	for (const auto& input : node->inputs)
	{
		if (input->name == name)
			return input.get();
	}
	return nullptr;
}

} // anonymous namespace

RenderGraph::NodeT* RenderGraph::AddPass(Pass* pass)
{
	if (!pass)
		throw std::runtime_error("RenderGraph::AddPass: pass must be non-null");

	const PassIO io = pass->GetIO();
	const std::vector<const ShaderGPU*> shaders = pass->GetShaders();
	PassPorts ports = CollectUploadedPassPorts(io.name.empty() ? "Pass" : io.name, shaders);
	return AddPass(pass, std::move(ports), PassExternalInputs(io.name));
}

RenderGraph::NodeT* RenderGraph::AddPass(Pass* pass, PassPorts ports,
                                         std::vector<std::string> externalInputs,
                                         std::string id)
{
	NodeT* node = MakePassNode(m_graph, pass, std::move(ports),
	                           std::move(externalInputs), std::move(id));
	m_isCompiled = false;
	m_compiled.clear();
	return node;
}

RenderGraph::NodeT* RenderGraph::MakePassNode(GraphT& graph, Pass* pass, PassPorts ports,
                                              std::vector<std::string> externalInputs,
                                              std::string id)
{
	if (!pass)
		throw std::runtime_error("RenderGraph::MakePassNode: pass must be non-null");

	PassBody entry;
	entry.pass           = pass;
	entry.io             = pass->GetIO();
	entry.ports          = std::move(ports);
	entry.externalInputs = std::move(externalInputs);

	std::string nodeName = entry.io.name.empty() ? std::string("Pass") : entry.io.name;
	entry.id             = id.empty() ? nodeName : std::move(id);
	entry.legacyExternal = entry.ports.ports.empty();
	auto* node = graph.AddNode(std::move(nodeName), std::move(entry));

	if (node->data.legacyExternal)
	{
		// Overlay passes still declare image I/O through PassIO. Their socket
		// names are the attachment names and any unconnected input is external.
		for (const auto& binding : node->data.io.reads)
		{
			auto* input = node->AddInput(AttachmentNameToString(binding.resource));
			input->data = PassPort{input->name, PortDirection::Input, "",
			                         binding.binding, 0};
		}
		for (const auto& binding : node->data.io.writes)
		{
			auto* output = node->AddOutput(AttachmentNameToString(binding.resource));
			output->data = PassPort{output->name, PortDirection::Output, "",
			                          binding.binding, 0};
		}
	}
	else
	{
		for (const PassPort& port : node->data.ports.ports)
		{
			if (port.direction == PortDirection::Input)
			{
				auto* input = node->AddInput(port.name);
				input->data = port;
			}
			else
			{
				auto* output = node->AddOutput(port.name);
				output->data = port;
			}
		}
	}

	return node;
}

void RenderGraph::Adopt(GraphT graph)
{
	m_graph = std::move(graph);
	m_compiled.clear();
	m_isCompiled = false;
}

bool RenderGraph::Connect(NodeT* producer, const std::string_view fromPort,
                          NodeT* consumer, const std::string_view toPort)
{
	if (!producer || !consumer)
		return false;

	SocketOutT* out = FindOutput(producer, fromPort);
	if (!out)
		return false;

	SocketInT* in = FindInput(consumer, toPort);
	if (!in)
		return false;

	const bool ok = m_graph.Connect(out, in);
	if (ok)
	{
		m_isCompiled = false;
		m_compiled.clear();
	}
	return ok;
}

bool RenderGraph::Connect(NodeT* producer, const AttachmentName resource, NodeT* consumer)
{
	const char* name = AttachmentNameToString(resource);
	return Connect(producer, name, consumer, name);
}

void RenderGraph::Compile()
{
	for (const auto& node : m_graph.nodes)
	{
		if (node->data.legacyExternal)
			continue;

		for (const auto& input : node->inputs)
		{
			if (input->source)
				continue;

			const auto& external = node->data.externalInputs;
			if (std::find(external.begin(), external.end(), input->name) != external.end())
				continue;

			throw std::runtime_error(
				"RenderGraph::Compile: node '" + node->name + "' input '"
				+ input->name + "' is not connected and is not external");
		}
	}

	// --- Binding plan: one image descriptor per connected input and per image output ---
	for (const auto& node : m_graph.nodes)
	{
		node->data.imageBindings.clear();
		if (node->data.legacyExternal)
			continue;

		const auto connected = [&node](const std::string_view port)
		{
			for (const auto& input : node->inputs)
			{
				if (input->name == port && input->source)
					return true;
			}
			return false;
		};

		node->data.imageBindings =
			BuildImageBindings(node->data.ports, *node->data.pass, connected);
	}

	m_compiled   = m_graph.TopologicalSort();
	m_isCompiled = true;
}

void RenderGraph::Execute(vk::CommandBuffer cmd,
                          RenderCache& cache,
                          const RenderContext& ctx,
                          GPUProfiler& profiler,
                          FrameProfile& frameProfile) const
{
	if (!m_isCompiled)
		throw std::runtime_error("RenderGraph::Execute: graph not compiled");

	// Profiling is driven here so passes stay unaware of it: each pass is
	// bracketed with GPU timestamps and its CPU-side Record() is timed.
	frameProfile.passes.clear();
	frameProfile.passes.reserve(m_compiled.size());
	frameProfile.drawCalls  = 0;
	frameProfile.dispatches = 0;

	double cpuRecordMs = 0.0;
	for (auto* node : m_compiled)
	{
		const std::string& name = node->data.io.name;

		// --- Resolve this node's images from the planned resource cache ---
		const vk::Extent2D renderExtent{ctx.width, ctx.height};
		PassResources resources;

		for (const auto& input : node->inputs)
		{
			if (node->data.legacyExternal || !input->source)
				continue;

			const std::string key =
				ResourceKey(input->source->owner->data.id, input->source->name);
			Image* image = cache.FindPlannedResource(key);
			if (!image)
			{
				const GraphOutputTag* tag = cache.GetResourceTag(key);
				if (tag && IsLogicalTag(*tag))
					continue;
				throw std::runtime_error(
					"RenderGraph::Execute: missing resource '" + key + "'");
			}
			resources.SetInput(input->name, image);
		}

		for (const auto& output : node->outputs)
		{
			if (node->data.legacyExternal)
				continue;

			const std::string key = ResourceKey(node->data.id, output->name);
			const GraphOutputTag* tag = cache.GetResourceTag(key);
			if (!tag)
				continue;  // An untagged output is unused and carries no image.

			if (IsLogicalTag(*tag))
				continue;

			resources.SetOutput(output->name, &cache.GetResource(key, renderExtent));
		}

		if (!node->data.legacyExternal)
		{
			WritePassBindings(*node->data.pass, cache, ctx, resources,
			                  node->data.imageBindings);
		}

		profiler.BeginPass(cmd, name);
		const auto cpuBegin = std::chrono::steady_clock::now();

		const PassFrame frame{cache, ctx, std::move(resources), ctx.frameIndex};
		const PassStats stats = node->data.pass->Record(cmd, frame);

		const auto cpuEnd = std::chrono::steady_clock::now();
		profiler.EndPass(cmd);

		const double passCpuMs =
			std::chrono::duration<double, std::milli>(cpuEnd - cpuBegin).count();
		cpuRecordMs += passCpuMs;

		PassProfile profile;
		profile.name       = name;
		profile.cpuMs      = passCpuMs;
		profile.gpuMs      = 0.0;
		profile.drawCalls  = stats.drawCalls;
		profile.dispatches = stats.dispatches;
		frameProfile.passes.push_back(std::move(profile));

		frameProfile.drawCalls  += stats.drawCalls;
		frameProfile.dispatches += stats.dispatches;
	}

	frameProfile.passCount   = static_cast<uint32_t>(frameProfile.passes.size());
	frameProfile.cpuRecordMs = cpuRecordMs;
}

void RenderGraph::Clear()
{
	m_graph.nodes.clear();
	m_compiled.clear();
	m_isCompiled = false;
}

void RenderGraph::Reset()
{
	m_compiled.clear();
	m_isCompiled = false;
}

} // namespace neurus
