#include "editor/controllers/RenderGraphController.hpp"

#include "asset/data/ShaderData.hpp"
#include "editor/RenderManager.hpp"
#include "editor/events/OperationEvents.hpp"
#include "editor/events/GraphEvents.hpp"
#include "editor/operations/GraphOperations.hpp"
#include "editor/operations/GraphTopologyOperations.hpp"
#include "asset/data/render_graph/CPUGraphValidation.hpp"
#include "shader/BuiltInRenderData.hpp"

#include <algorithm>
#include <stdexcept>

namespace neurus {
namespace {

template<class Event, class Edit>
void SubscribeGraphEdit(ControllerContext& ctx, Edit edit)
{
	ctx.events.Subscribe<Event>([&ctx, edit](const Event& request) {
		try
		{
			// Topology requests address the sockets exposed by earlier shader writes.
			ctx.events.Emit(RenderGraphFlushRequested{});
			edit(request);
		}
		catch (const std::exception& failure)
		{
			ctx.events.Emit(OperationRejected{failure.what()});
			if (!request.error)
				throw;
			*request.error = failure.what();
		}
	});
}

using NodeType = RenderGraphData::NodeType;

RenderGraphData& Document(const ControllerContext& ctx)
{
	const auto& graph = ctx.render->GraphData();
	if (!graph)
		throw std::logic_error("Create a project render graph before editing the built-in graph");
	return *graph;
}

RenderGraphData& Document(const ControllerContext& ctx, int uid)
{
	auto& graph = Document(ctx);
	if (graph.GetObjectID() != uid)
		throw std::invalid_argument("Graph history refers to a different active document");
	return graph;
}

auto NodePosition(const RenderGraphData& graph, const std::string& name)
{
	return std::ranges::find_if(graph.nodes, [&](const auto& node) { return node->name == name; });
}

NodeType& FindNode(const RenderGraphData& graph, const std::string& name)
{
	const auto found = NodePosition(graph, name);
	if (found == graph.nodes.end())
		throw std::invalid_argument("Unknown graph node '" + name + "'");
	return **found;
}

void CheckNewName(const RenderGraphData& graph, const std::string& name)
{
	if (name.empty() || name.find('/') != std::string::npos)
		throw std::invalid_argument("A graph node name must be nonempty and cannot contain '/'");
	if (NodePosition(graph, name) != graph.nodes.end())
		throw std::invalid_argument("Duplicate graph node '" + name + "'");
}

template<class Sockets>
auto& FindSocket(const Sockets& sockets, const GraphEndpoint& endpoint)
{
	for (const auto& socket : sockets)
		if (socket->name == endpoint.port)
			return *socket;
	throw std::invalid_argument("Unknown graph socket '" + endpoint.node + "." + endpoint.port + "'");
}

auto& Output(const RenderGraphData& graph, const GraphEndpoint& endpoint)
{
	return FindSocket(FindNode(graph, endpoint.node).outputs, endpoint);
}

auto& Input(const RenderGraphData& graph, const GraphEndpoint& endpoint)
{
	return FindSocket(FindNode(graph, endpoint.node).inputs, endpoint);
}

template<class Socket>
GraphEndpoint Endpoint(const Socket& socket)
{
	return {socket.owner->name, socket.name};
}

// A destination with an empty node names a public scene output.
std::optional<GraphEndpoint> Producer(const RenderGraphData& graph, const GraphEndpoint& destination)
{
	if (destination.node.empty())
	{
		if (std::ranges::find(kSceneOutputNames, destination.port) == kSceneOutputNames.end())
			throw std::invalid_argument("Unknown scene output '" + destination.port + "'");
		const auto it = graph.sceneOutputs.find(destination.port);
		if (it != graph.sceneOutputs.end())
			return it->second;
	}
	else if (const auto* source = Input(graph, destination).incoming.from)
		return Endpoint(*source);
	return {};
}

std::unique_ptr<Operation> ComposeEdits(std::vector<std::unique_ptr<Operation>> edits, const char* label)
{
	if (edits.size() == 1)
		return std::move(edits.front());
	return std::make_unique<NamedCompositeOp>(std::move(edits), label);
}

void ChangeProducer(const ControllerContext& ctx, const GraphProducerChanged& e)
{
	auto& graph = Document(ctx, e.graphUid);
	if (Producer(graph, e.destination) != e.before)
		throw std::invalid_argument("Unexpected producer for graph socket '" + e.destination.port + "'");
	auto* output = e.after ? &Output(graph, *e.after) : nullptr;
	if (e.destination.node.empty())
	{
		if (e.after)
			graph.sceneOutputs.insert_or_assign(e.destination.port, *e.after);
		else
			graph.sceneOutputs.erase(e.destination.port);
		return;
	}

	auto& input = Input(graph, e.destination);
	if (!output)
		graph.Disconnect(input.incoming.from, &input);
	else if (!graph.Connect(output, &input))
		throw std::invalid_argument("Connection to '" + e.destination.node + "." + e.destination.port
		                            + "' creates a cycle");
}

void SetProducer(const ControllerContext& ctx, const GraphEndpoint& destination, std::optional<GraphEndpoint> source)
{
	const auto& graph = Document(ctx);
	const auto old = Producer(graph, destination);
	if (old == source)
		return;

	// A replacement emits one producer change. History records unlink then link so undo restores the old producer.
	const int uid = graph.GetObjectID();
	std::vector<std::unique_ptr<Operation>> edits;
	if (old)
		edits.push_back(std::make_unique<GraphConnectionOp>(uid, *old, destination, false));
	if (source)
		edits.push_back(std::make_unique<GraphConnectionOp>(uid, *source, destination, true));
	auto history = ComposeEdits(std::move(edits), "Replace Graph Connection");
	ctx.events.Emit(GraphProducerChanged{uid, destination, old, source});
	ctx.ops.Submit(std::move(history));
}

void ChangeNodeMembership(const ControllerContext& ctx, const GraphNodeMembershipChanged& e)
{
	auto& graph = Document(ctx, e.graphUid);
	if (!e.present)
	{
		graph.RemoveNode(&FindNode(graph, e.node.name));
		std::erase_if(graph.sceneOutputs, [&](const auto& item) { return item.second.node == e.node.name; });
		return;
	}
	CheckNewName(graph, e.node.name);
	if (e.index > graph.nodes.size())
		throw std::invalid_argument("Invalid graph node position in history");
	auto node = project::FromRecord(e.node, ctx.resources);
	if (e.node.shaderUid && !node->data.shader)
		throw std::invalid_argument("Missing or invalid pooled shader UID " + std::to_string(*e.node.shaderUid));
	if (node->data.shader)
		ValidatePassShader(node->data.pass, *node->data.shader, ctx.render->Defaults());
	graph.nodes.insert(graph.nodes.begin() + e.index, std::move(node));
}

void RemoveNode(const ControllerContext& ctx, const std::string& name)
{
	auto& graph = Document(ctx);
	const auto& node = FindNode(graph, name);
	const int uid = graph.GetObjectID();
	// Record the disconnects first so undo recreates the node before restoring its links.
	std::vector<std::unique_ptr<Operation>> ops;
	for (const auto& input : node.inputs)
		if (input->IsConnected())
			ops.push_back(std::make_unique<GraphConnectionOp>(uid, Endpoint(*input->incoming.from), Endpoint(*input), false));
	for (const auto& output : node.outputs)
		for (const auto* target : output->targets)
			ops.push_back(std::make_unique<GraphConnectionOp>(uid, Endpoint(*output), Endpoint(*target), false));
	for (const auto& [output, source] : graph.sceneOutputs)
		if (source.node == name)
			ops.push_back(std::make_unique<GraphConnectionOp>(uid, source, GraphEndpoint{{}, output}, false));

	const auto index = static_cast<size_t>(NodePosition(graph, name) - graph.nodes.begin());
	const auto record = project::ToRecord(node);
	ops.push_back(std::make_unique<GraphNodeOp>(uid, record, index, false));
	auto history = ComposeEdits(std::move(ops), "Remove Graph Node");
	ctx.events.Emit(GraphNodeMembershipChanged{uid, record, index, false});
	ctx.ops.Submit(std::move(history));
}

void ChangeNodeShader(const ControllerContext& ctx, const GraphNodeShaderChanged& e)
{
	auto& graph = Document(ctx, e.graphUid);
	auto& node = FindNode(graph, e.node);
	auto data = node.data;
	data.shader = nullptr;
	if (e.shaderUid)
	{
		data.shader = ctx.resources.Get<const ShaderData>(e.shaderUid);
		if (!data.shader)
			throw std::invalid_argument("Unknown pooled shader UID " + std::to_string(e.shaderUid));
	}
	if (data.shader)
		ValidatePassShader(data.pass, *data.shader, ctx.render->Defaults());
	ReconcilePassPorts(graph, node, EffectivePassPorts(data, ctx.render->Defaults().Ports()));
	node.data.shader = std::move(data.shader);
}

} // namespace

void RenderGraphController::Init(ControllerContext& ctx)
{
	// Live requests and history replay share these document mutation handlers.
	ctx.events.Subscribe<GraphProducerChanged>([&ctx](const auto& e) { ChangeProducer(ctx, e); });
	ctx.events.Subscribe<GraphNodeMembershipChanged>([&ctx](const auto& e) { ChangeNodeMembership(ctx, e); });
	ctx.events.Subscribe<GraphNodeShaderChanged>([&ctx](const auto& e) { ChangeNodeShader(ctx, e); });
	ctx.events.Subscribe<GraphNodeRenamed>([&ctx](const auto& e) {
		auto& graph = Document(ctx, e.graphUid);
		auto& node = FindNode(graph, e.from);
		CheckNewName(graph, e.to);
		node.name = e.to;
		for (auto& [name, source] : graph.sceneOutputs)
			if (source.node == e.from)
				source.node = e.to;
	});
	ctx.events.Subscribe<GraphOutputTagChanged>([&ctx](const auto& e) {
		Output(Document(ctx, e.graphUid), e.output).data.tag = e.tag;
	});

	SubscribeGraphEdit<RenderGraphAddRequested>(ctx, [&ctx](const auto& e) {
		auto& graph = Document(ctx);
		const auto& defaults = ctx.render->Defaults().Graph()->nodes;
		const auto found = std::ranges::find_if(defaults, [&](const auto& n) { return n->data.pass == e.kind; });
		if (found == defaults.end())
			throw std::invalid_argument("Unknown pass kind '" + e.kind + "'");
		auto node = project::ToRecord(**found);
		std::string id = e.id;
		if (id.empty())
		{
			id = node.name;
			for (int i = 2; NodePosition(graph, id) != graph.nodes.end(); ++i)
				id = node.name + "_" + std::to_string(i);
		}
		node.name = id;
		const auto index = graph.nodes.size();
		ctx.events.Emit(GraphNodeMembershipChanged{graph.GetObjectID(), node, index, true});
		ctx.ops.Submit(std::make_unique<GraphNodeOp>(graph.GetObjectID(), std::move(node), index, true));
		if (e.createdId)
			*e.createdId = std::move(id);
	});
	SubscribeGraphEdit<RenderGraphRemoveRequested>(ctx, [&ctx](const auto& e) { RemoveNode(ctx, e.node); });
	SubscribeGraphEdit<RenderGraphRenameRequested>(ctx, [&ctx](const auto& e) {
		const auto& graph = Document(ctx);
		FindNode(graph, e.node);
		if (e.node == e.name)
			return;
		ctx.events.Emit(GraphNodeRenamed{graph.GetObjectID(), e.node, e.name});
		ctx.ops.Submit(std::make_unique<RenameGraphNodeOp>(graph.GetObjectID(), e.node, e.name));
	});
	SubscribeGraphEdit<RenderGraphConnectRequested>(ctx, [&ctx](const auto& e) { SetProducer(ctx, e.destination, e.source); });
	SubscribeGraphEdit<RenderGraphDisconnectRequested>(ctx, [&ctx](const auto& e) { SetProducer(ctx, e.destination, {}); });
	SubscribeGraphEdit<RenderGraphTagRequested>(ctx, [&ctx](const auto& e) {
		const auto& graph = Document(ctx);
		const auto previous = Output(graph, e.output).data.tag;
		if (previous == e.tag)
			return;
		ctx.events.Emit(GraphOutputTagChanged{graph.GetObjectID(), e.output, e.tag});
		ctx.ops.Submit(std::make_unique<SetGraphOutputTagOp>(graph.GetObjectID(), e.output, previous, e.tag));
	});
	SubscribeGraphEdit<RenderGraphShaderRequested>(ctx, [&ctx](const auto& e) {
		const auto& graph = Document(ctx);
		const auto& node = FindNode(graph, e.node);
		const auto shader = e.uid ? ctx.resources.Get<ShaderData>(*e.uid) : nullptr;
		if (e.uid && !shader)
			throw std::invalid_argument("Unknown pooled shader UID " + std::to_string(*e.uid));
		if (node.data.shader.get() == shader.get())
			return;
		const int before = node.data.shader.id();
		const int after = e.uid.value_or(0);
		ctx.events.Emit(GraphNodeShaderChanged{graph.GetObjectID(), e.node, after});
		ctx.ops.Submit(std::make_unique<SetGraphShaderOp>(graph.GetObjectID(), e.node, before, after));
	});
	SubscribeGraphEdit<RenderGraphCreateRequested>(ctx, [&ctx](const auto&) {
		if (!ctx.render->GraphData())
		{
			auto graph = ctx.render->CreateGraph(false);
			ctx.ops.Submit(std::make_unique<CreateRenderGraphOp>(graph->GetObjectID()));
		}
	});
	SubscribeGraphEdit<RenderGraphCreateShaderRequested>(ctx, [&ctx](const auto& request) {
		const auto& graph = Document(ctx);
		const auto& node = FindNode(graph, request.node);
		if (!node.data.shader)
		{
			auto shader = ctx.render->Defaults().CreatePassShader(node.data.pass, node.name + " Shader");
			ctx.render->RegisterShader(shader);
			const int uid = shader->GetObjectID();
			try
			{
				ctx.events.Emit(GraphNodeShaderChanged{graph.GetObjectID(), node.name, uid});
			}
			catch (...)
			{
				ctx.render->DiscardCreation(uid);
				throw;
			}
			ctx.ops.Submit(std::make_unique<SetGraphShaderOp>(graph.GetObjectID(), node.name, 0, uid, true));
		}
		if (request.uid)
			*request.uid = node.data.shader.id();
	});

	ctx.events.Subscribe<GraphCreationDiscarded>([&ctx](const auto& e) { ctx.render->DiscardCreation(e.uid); });
	ctx.events.Subscribe<RenderGraphMembershipChanged>([&ctx](const RenderGraphMembershipChanged& e) {
		const auto active = ctx.render->GraphData();
		if (active && active->GetObjectID() != e.uid)
			throw std::runtime_error("Graph creation history refers to a different active graph");
		if (e.present)
		{
			if (active)
				return;
			auto graph = ctx.resources.Get<RenderGraphData>(e.uid);
			if (!graph)
				throw std::runtime_error("Graph creation history refers to a missing resource");
			ctx.render->GraphData() = std::move(graph);
		}
		else
		{
			if (!active)
				throw std::runtime_error("Cannot undo graph creation without its active resource");
			ctx.render->ResetGraph(false);
		}
	});
}

} // namespace neurus
