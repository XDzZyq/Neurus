/**
 * @file RenderGraph.cpp
 * @brief Implementation of RenderGraph.
 */

#include "RenderGraph.hpp"
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
	// The legacy path materializes PassIO sockets. A pass with uploaded shaders
	// needs its port table, so reject rather than silently drop its ports.
	if (!pass->GetShaders().empty())
		throw std::runtime_error("RenderGraph::AddPass: a pass with uploaded shaders requires explicit ports");

	const PassIO io = pass->GetIO();
	return AddPass(pass, {}, PassExternalInputs(io.name));
}

RenderGraph::NodeT* RenderGraph::AddPass(Pass* pass, const CPUPorts& ports,
                                         std::vector<std::string> externalInputs,
                                         std::string id)
{
	if (!pass)
		throw std::runtime_error("RenderGraph::AddPass: pass must be non-null");

	auto body = MakePassBody(*pass, std::move(externalInputs), std::move(id), ports.ports.empty());
	auto nodeName = body.io.name.empty() ? std::string("Pass") : body.io.name;
	auto* node = m_graph.AddNode(std::move(nodeName), std::move(body));
	m_isCompiled = false;
	m_compiled.clear();

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
		for (const CPUPort& port : ports.ports)
		{
			if (port.direction == PortDirection::Input)
			{
				auto* input = node->AddInput(port.name);
				input->data = PassPort::FromCPUPort(port);
			}
			else
			{
				auto* output = node->AddOutput(port.name);
				output->data = PassPort::FromCPUPort(port);
			}
		}
	}

	return node;
}

RenderGraph::PassBody RenderGraph::MakePassBody(Pass& pass, std::vector<std::string> externalInputs,
                                                std::string id, bool legacyExternal)
{
	PassBody body;
	body.pass = &pass;
	body.io = pass.GetIO();
	body.id = id.empty() ? (body.io.name.empty() ? "Pass" : body.io.name) : std::move(id);
	body.externalInputs = std::move(externalInputs);
	body.legacyExternal = legacyExternal;
	return body;
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

void RenderGraph::Compile(const std::set<std::string>* active)
{
	m_compiled = m_graph.TopologicalSort();
	if (active)
		std::erase_if(m_compiled, [&](const NodeT* node) { return !active->contains(node->name); });
	for (auto* node : m_compiled)
	{
		if (node->data.legacyExternal)
			continue;

		for (const auto& input : node->inputs)
		{
			if (input->IsConnected())
				continue;

			const auto& external = node->data.externalInputs;
			if (std::find(external.begin(), external.end(), input->data.name) != external.end())
				continue;

			throw std::runtime_error(
				"RenderGraph::Compile: node '" + node->name + "' input '"
				+ input->name + "' is not connected and is not external");
		}
	}

	// --- Binding plan: one image descriptor per connected input and per image output ---
	for (auto* node : m_compiled)
	{
		node->data.imageBindings.clear();
		if (node->data.legacyExternal)
			continue;

		const auto selfManaged = node->data.pass->SelfManagedBindings();
		const auto append = [&](const PassPort& port)
		{
			AppendImageBinding(node->data.imageBindings, port.name, port.direction, port.type,
			                   port.binding, selfManaged);
		};
		for (const auto& input : node->inputs)
			if (input->IsConnected())
				append(input->data);
		for (const auto& output : node->outputs)
			append(output->data);
	}

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
			if (node->data.legacyExternal || !input->IsConnected())
				continue;

			const std::string key =
				ResourceKey(input->incoming.from->owner->data.id, input->incoming.from->name);
			Image* image = cache.FindPlannedResource(key);
			if (!image)
			{
				const GraphOutputTag* tag = cache.GetResourceTag(key);
				if (tag && IsLogicalTag(*tag))
					continue;
				throw std::runtime_error(
					"RenderGraph::Execute: missing resource '" + key + "'");
			}
			resources.SetInput(input->data.name, image);
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

			resources.SetOutput(output->data.name, &cache.GetResource(key, renderExtent));
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
