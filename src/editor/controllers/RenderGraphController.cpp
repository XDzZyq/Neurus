#include "editor/controllers/RenderGraphController.hpp"

#include "asset/data/ShaderData.hpp"
#include "editor/RenderManager.hpp"
#include "editor/controllers/MutationNotify.hpp"
#include "editor/events/GraphEvents.hpp"
#include "editor/events/ScriptEvents.hpp"
#include "editor/events/ShaderEvents.hpp"
#include "editor/operations/GraphOperations.hpp"
#include "editor/operations/IOperationSink.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "shader/ShaderLibrary.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace neurus {
namespace {

using PassNode = RenderGraphTopology::NodeType;

PassNode& FindNode(RenderGraphTopology& graph, const std::string& name)
{
	for (auto& node : graph.nodes)
		if (node->name == name)
			return *node;
	throw std::invalid_argument("Unknown graph node '" + name + "'");
}

template<class Sockets>
auto& FindSocket(Sockets& sockets, const GraphEndpoint& endpoint)
{
	for (auto& socket : sockets)
		if (socket->name == endpoint.port)
			return *socket;
	throw std::invalid_argument("Unknown graph socket '" + endpoint.node + "." + endpoint.port + "'");
}

void CheckOutput(const std::string& name)
{
	if (std::ranges::find(kSceneOutputNames, name) == kSceneOutputNames.end())
		throw std::invalid_argument("Unknown scene output '" + name + "'");
}

bool HasNode(const RenderGraphTopology& graph, const std::string& name)
{
	return std::ranges::any_of(graph.nodes, [&](const auto& node) { return node->name == name; });
}

void CheckName(const std::string& name)
{
	if (name.empty() || name.find('/') != std::string::npos)
		throw std::invalid_argument("A graph node name must be nonempty and cannot contain '/'");
}

} // namespace

void RenderGraphController::Init(ControllerContext& ctx)
{
	// Register before ScriptController so final validation and history submission
	// happen inside the existing script history scope.
	ctx.events.Subscribe<ScriptRunStarted>([this](const auto&) { m_script = true; });
	ctx.events.Subscribe<ScriptRunFinished>([this, &ctx](const ScriptRunFinished& e) {
		m_script = false;
		try
		{
			if (!e.success)
				Rollback(ctx);
			else
				Commit(ctx);
		}
		catch (const std::exception& error)
		{
			const std::string message = error.what();
			Rollback(ctx);
			if (e.error)
				*e.error = message;
			else
				throw;
		}
	});
	ctx.events.Subscribe<RenderGraphFlushRequested>([this, &ctx](const RenderGraphFlushRequested& e) {
		FlushShaders(ctx, e.diagnostics);
	});
	ctx.events.Subscribe<RenderGraphStageEditRequested>([this, &ctx](const RenderGraphStageEditRequested& e) {
		e.handled = EditShader(ctx, e.uid, e.stage, std::move(e.candidate), e.fromStruct);
	});
	ctx.events.Subscribe<RenderGraphCreateRequested>([this, &ctx](const auto&) {
		Edit(ctx, [&] {
			if (!ctx.render->GraphData())
			{
				ctx.render->CreateGraph(false);
				m_created = true;
			}
		});
	});
	ctx.events.Subscribe<RenderGraphAddRequested>([this, &ctx](const RenderGraphAddRequested& e) {
		Edit(ctx, [&] {
			auto& graph = Document(ctx);
			const auto& defaults = *ctx.render->Defaults().Graph();
			const auto found = std::ranges::find_if(defaults.nodes, [&](const auto& n) { return n->data.pass == e.kind; });
			if (found == defaults.nodes.end())
				throw std::invalid_argument("Unknown pass kind '" + e.kind + "'");
			const auto& source = **found;
			std::string id = e.id;
			if (id.empty())
			{
				id = source.name;
				for (int i = 2; HasNode(graph, id); ++i)
					id = source.name + "_" + std::to_string(i);
			}
			CheckName(id);
			if (HasNode(graph, id))
				throw std::invalid_argument("Duplicate graph node '" + id + "'");
			auto* node = graph.AddNode(id, source.data);
			for (const auto& socket : source.inputs)
				node->AddInput(socket->name)->data = socket->data;
			for (const auto& socket : source.outputs)
				node->AddOutput(socket->name)->data = socket->data;
			if (e.createdId)
				*e.createdId = id;
		});
	});
	ctx.events.Subscribe<RenderGraphRemoveRequested>([this, &ctx](const RenderGraphRemoveRequested& e) {
		Edit(ctx, [&] {
			auto& graph = Document(ctx);
			graph.RemoveNode(&FindNode(graph, e.node));
			std::erase_if(graph.sceneOutputs, [&](const auto& item) { return item.second.node == e.node; });
		});
	});
	ctx.events.Subscribe<RenderGraphRenameRequested>([this, &ctx](const RenderGraphRenameRequested& e) {
		Edit(ctx, [&] {
			auto& graph = Document(ctx);
			auto& node = FindNode(graph, e.node);
			CheckName(e.name);
			if (e.node != e.name && HasNode(graph, e.name))
				throw std::invalid_argument("Duplicate graph node '" + e.name + "'");
			node.name = e.name;
			for (auto& [name, endpoint] : graph.sceneOutputs)
				if (endpoint.node == e.node)
					endpoint.node = e.name;
		});
	});
	ctx.events.Subscribe<RenderGraphConnectRequested>([this, &ctx](const RenderGraphConnectRequested& e) {
		Edit(ctx, [&] {
			auto& graph = Document(ctx);
			auto& output = FindSocket(FindNode(graph, e.source.node).outputs, e.source);
			if (e.destination.node.empty())
			{
				CheckOutput(e.destination.port);
				graph.sceneOutputs[e.destination.port] = e.source;
				return;
			}
			auto& input = FindSocket(FindNode(graph, e.destination.node).inputs, e.destination);
			if (input.source != &output && !graph.Connect(&output, &input))
				throw std::invalid_argument("Connection to '" + e.destination.node + "." + e.destination.port
				                            + "' creates a cycle");
		});
	});
	ctx.events.Subscribe<RenderGraphDisconnectRequested>([this, &ctx](const RenderGraphDisconnectRequested& e) {
		Edit(ctx, [&] {
			auto& graph = Document(ctx);
			if (e.destination.node.empty())
			{
				CheckOutput(e.destination.port);
				graph.sceneOutputs.erase(e.destination.port);
				return;
			}
			auto& input = FindSocket(FindNode(graph, e.destination.node).inputs, e.destination);
			if (input.source)
				graph.Disconnect(input.source, &input);
		});
	});
	ctx.events.Subscribe<RenderGraphTagRequested>([this, &ctx](const RenderGraphTagRequested& e) {
		Edit(ctx, [&] { FindSocket(FindNode(Document(ctx), e.output.node).outputs, e.output).data.tag = e.tag; });
	});
	ctx.events.Subscribe<RenderGraphShaderRequested>([this, &ctx](const RenderGraphShaderRequested& e) {
		Edit(ctx, [&] {
			auto& node = FindNode(Document(ctx), e.node);
			auto shader = e.uid ? ctx.resources.Get<ShaderData>(*e.uid) : nullptr;
			if (e.uid && !shader)
				throw std::invalid_argument("Unknown pooled shader UID " + std::to_string(*e.uid));
			AssignShader(ctx, node, std::move(shader));
		});
	});
	ctx.events.Subscribe<RenderGraphCreateShaderRequested>([this, &ctx](const RenderGraphCreateShaderRequested& e) {
		Edit(ctx, [&] {
			auto& node = FindNode(Document(ctx), e.node);
			if (!node.data.shader)
			{
				auto shader = ctx.render->Defaults().CreatePassShader(node.data.pass, node.name + " Shader");
				ctx.render->RegisterShader(shader);
				m_edit.createdShaders.push_back(shader->GetObjectID());
				AssignShader(ctx, node, std::move(shader));
			}
			if (e.uid)
				*e.uid = node.data.shader->GetObjectID();
		});
	});
	ctx.events.Subscribe<RenderGraphCreationRestored>([&ctx](const RenderGraphCreationRestored& e) {
		const auto active = ctx.render->GraphData();
		if (active && active->GetObjectID() != e.uid)
			throw std::runtime_error("Graph creation history refers to a different active graph");
		if (e.present)
		{
			if (active)
				return;
			if (!e.detached || e.detached->GetObjectID() != e.uid)
				throw std::runtime_error("Cannot redo graph creation without its detached resource");
			ctx.render->RestoreCreatedGraph(e.detached);
			e.detached.reset();
		}
		else
		{
			if (!active)
				throw std::runtime_error("Cannot undo graph creation without its active resource");
			ctx.render->RemoveCreatedGraph(active);
			e.detached = active;
		}
		notify::Mutated(ctx.events);
	});
	ctx.events.Subscribe<RenderGraphEditRestored>([this, &ctx](const RenderGraphEditRestored& e) {
		Replay(ctx, e.state, e.forward);
	});
}

void RenderGraphController::Edit(const ControllerContext& ctx, const std::function<void()>& action)
{
	try
	{
		action();
		if (!m_script)
			Commit(ctx);
	}
	catch (const std::exception& error)
	{
		if (m_script)
			m_error = error.what();
		else
			Rollback(ctx);
		throw;
	}
}

RenderGraphData& RenderGraphController::Document(const ControllerContext& ctx)
{
	FlushShaders(ctx);
	const auto& graph = ctx.render->GraphData();
	if (!graph)
		throw std::logic_error("Call neurus.render.graph.create() before editing the built-in graph");
	if (!m_original)
		m_original = CloneGraph(ctx.render->Graph());
	return *graph;
}

void RenderGraphController::AssignShader(const ControllerContext& ctx, PassNode& node, std::shared_ptr<ShaderData> shader)
{
	if (node.data.shader == shader)
		return;
	if (shader)
		ctx.render->Defaults().ValidatePassShader(node.data.pass, *shader);
	node.data.shader = std::move(shader);
	const auto ports = EffectivePassPorts(node.data, ctx.render->Defaults().Ports());
	ReconcilePassPorts(Document(ctx), node, ports);
}

bool RenderGraphController::EditShader(const ControllerContext& ctx, int uid, ShaderType stage, ShaderUnit candidate, bool fromStruct)
{
	if (!ctx.render->ReferencesShader(uid))
		return false;
	Edit(ctx, [&] {
		if (!m_original)
			m_original = CloneGraph(ctx.render->Graph());
		auto shader = ctx.resources.Get<ShaderData>(uid);
		if (!shader || !shader->HasStage(stage))
			throw std::invalid_argument("Unknown shader stage for UID " + std::to_string(uid));
		auto& current = shader->GetStage(stage);
		if (!fromStruct && candidate.code == current.code && !current.spv.empty())
			return;
		auto change = std::ranges::find_if(m_edit.stages, [&](const auto& s) { return s.uid == uid && s.stage == stage; });
		if (change == m_edit.stages.end())
			m_edit.stages.push_back({uid, stage, current, candidate});
		else
			change->after = candidate;
		current = std::move(candidate);
		m_pendingStages[{uid, stage}] = fromStruct;
	});
	return true;
}

std::optional<CpuGraphDiagnostic> RenderGraphController::CompilePendingStages(const ControllerContext& ctx)
{
	if (m_pendingStages.empty())
		return std::nullopt;
	auto& graph = *ctx.render->GraphData();
	for (auto& change : m_edit.stages)
	{
		const auto pending = m_pendingStages.find({change.uid, change.stage});
		if (pending == m_pendingStages.end())
			continue;
		const auto shader = ctx.resources.Get<ShaderData>(change.uid);
		auto& current = shader->GetStage(change.stage);
		const auto& original = change.before;
		try
		{
			if (!pending->second && current.code == original.code)
				current = original;
			else
			{
				auto compiled = current;
				ShaderLibrary::CompileEditedUnit(compiled, change.stage, shader->GetName(), pending->second);
				compiled.m_version = original.m_version + (compiled.code != original.code || compiled.spv != original.spv);
				current = std::move(compiled);
			}
			for (auto& node : graph.nodes)
				if (node->data.shader == shader)
					ReconcilePassPorts(graph, *node, EffectivePassPorts(node->data, ctx.render->Defaults().Ports()));
		}
		catch (const std::exception& error)
		{
			const auto node = std::ranges::find_if(graph.nodes, [&](const auto& n) { return n->data.shader == shader; });
			const auto name = node == graph.nodes.end() ? shader->GetName() : (*node)->name;
			return CpuGraphDiagnostic{name, ShaderData::TypeToString(change.stage), error.what()};
		}
		change.after = current;
		m_pendingStages.erase(pending);
	}
	return std::nullopt;
}

void RenderGraphController::FlushShaders(const ControllerContext& ctx, std::vector<CpuGraphDiagnostic>* diagnostics)
{
	if (auto error = CompilePendingStages(ctx))
	{
		if (diagnostics)
			diagnostics->push_back(std::move(*error));
		else
			throw std::invalid_argument(error->node + "." + error->port + ": " + error->message);
	}
}

void RenderGraphController::PublishShaderEdits(const ControllerContext& ctx, const GraphEditState& state, bool topologyChanged)
{
	std::set<int> changed;
	for (const auto& edit : state.stages)
		changed.insert(edit.uid);
	for (int uid : changed)
		ctx.resources.Get<ShaderData>(uid)->BumpVersion();
	if (topologyChanged)
		ctx.render->Publish();
	for (int uid : changed)
	{
		if (!topologyChanged || !ctx.render->ReferencesShader(uid))
			ctx.render->UploadShader(*ctx.resources.Get<ShaderData>(uid));
		ctx.events.Emit(ShaderCompiled{uid, true});
	}
	notify::Mutated(ctx.events);
}

void RenderGraphController::Validate(const ControllerContext& ctx) const
{
	const auto diagnostics = ctx.render->Diagnostics();
	if (!diagnostics.empty())
	{
		const auto& d = diagnostics.front();
		const auto location = d.port.empty() ? d.node : d.node + "." + d.port;
		throw std::invalid_argument(location + ": " + d.message);
	}
}

void RenderGraphController::Commit(const ControllerContext& ctx)
{
	if (!m_error.empty())
		throw std::runtime_error(m_error);
	if (!m_original && !m_created)
		return;
	auto& graph = *ctx.render->GraphData();
	FlushShaders(ctx);
	std::erase_if(m_edit.stages, [](const auto& change) {
		return change.before.code == change.after.code && change.before.spv == change.after.spv;
	});
	m_edit.graphUid = graph.GetObjectID();
	Validate(ctx);
	if (m_original)
	{
		auto before = project::ToRecord(*m_original);
		auto after = project::ToRecord(graph);
		if (before != after)
		{
			m_edit.before = std::move(before);
			m_edit.after = std::move(after);
		}
	}
	const bool edited = m_edit.before || !m_edit.stages.empty() || !m_edit.createdShaders.empty();
	if (m_created || edited)
	{
		PublishShaderEdits(ctx, m_edit, m_created || m_edit.before.has_value());
		if (m_created)
			ctx.ops.Submit(std::make_unique<CreateRenderGraphOp>(graph.GetObjectID()));
		if (edited)
			ctx.ops.Submit(std::make_unique<EditRenderGraphOp>(std::move(m_edit)));
	}
	Clear();
}

void RenderGraphController::Clear()
{
	m_created = false;
	m_original.reset();
	m_edit = {};
	m_pendingStages.clear();
	m_error.clear();
}

void RenderGraphController::Rollback(const ControllerContext& ctx)
{
	for (const auto& change : m_edit.stages)
		ctx.resources.Get<ShaderData>(change.uid)->GetStage(change.stage) = change.before;
	if (m_original)
		static_cast<RenderGraphTopology&>(*ctx.render->GraphData()) = std::move(*m_original);
	if (m_created)
		ctx.render->RemoveCreatedGraph(ctx.render->GraphData(), false);
	for (int uid : m_edit.createdShaders)
		ctx.render->RemoveUnusedShader(uid);
	Clear();
}

void RenderGraphController::Replay(const ControllerContext& ctx, GraphEditState& state, bool forward)
{
	const auto& active = ctx.render->GraphData();
	if (!active || active->GetObjectID() != state.graphUid)
		throw std::runtime_error("Graph history refers to a different active document");
	try
	{
		auto& graph = Document(ctx);
		if (forward)
		{
			for (const auto& resource : state.detached)
			{
				auto shader = std::dynamic_pointer_cast<ShaderData>(resource);
				if (!shader || ctx.resources.Get(shader->GetObjectID()))
					throw std::runtime_error("Invalid shader identity in graph history");
				ctx.render->RegisterShader(shader);
				m_edit.createdShaders.push_back(shader->GetObjectID());
			}
		}
		for (const auto& change : state.stages)
		{
			const auto shader = ctx.resources.Get<ShaderData>(change.uid);
			if (!shader || !shader->HasStage(change.stage))
				throw std::runtime_error("Missing shader stage in graph history");
			auto target = forward ? change.after : change.before;
			if (target.spv.empty())
				ShaderLibrary::CompileEditedUnit(target, change.stage, shader->GetName(), false);
			auto& current = shader->GetStage(change.stage);
			target.m_version = current.m_version + 1;
			m_edit.stages.push_back({change.uid, change.stage, current, target});
			current = std::move(target);
		}
		if (const auto& record = forward ? state.after : state.before)
		{
			std::vector<std::string> errors;
			auto topology = project::FromRecord(*record, ctx.render->Defaults().Ports(), errors, &ctx.resources);
			if (!errors.empty())
				throw std::runtime_error("Invalid graph history: " + errors.front());
			static_cast<RenderGraphTopology&>(graph) = std::move(topology);
		}
		Validate(ctx);
		PublishShaderEdits(ctx, state, state.before.has_value());
		if (!forward)
		{
			for (int uid : state.createdShaders)
			{
				if (auto shader = ctx.render->RemoveUnusedShader(uid))
					state.detached.push_back(std::move(shader));
			}
		}
		else
			state.detached.clear();
		Clear();
	}
	catch (...)
	{
		Rollback(ctx);
		throw;
	}
}

} // namespace neurus
