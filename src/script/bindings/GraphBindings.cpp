#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "editor/events/GraphEvents.hpp"
#include "script/bindings/Bindings.hpp"
#include "script/proxies/ShaderProxies.hpp"

namespace py = pybind11;

namespace neurus::script {

namespace {

using CpuGraphNode = RenderGraphData::NodeType;

// A fresh generation after remove protects a saved handle even if the allocator
// reuses the old node's address for another node with the same name.
struct NodeGenerations
{
	std::size_t Get(const std::string& name, std::uint64_t project)
	{
		if (projectEpoch != project)
		{
			active.clear();
			projectEpoch = project;
		}
		auto [it, inserted] = active.try_emplace(name, next);
		if (inserted)
			++next;
		return it->second;
	}

	std::uint64_t projectEpoch = 0;
	std::size_t next = 0;
	std::map<std::string, std::size_t> active;
};

struct GraphProxy : ProxyBase
{
	std::shared_ptr<NodeGenerations> generations;

	ScriptGraphView Read() const
	{
		if (view && view->ctx && view->ctx->graphProvider)
			return view->ctx->graphProvider();
		return {};
	}

	const RenderGraphData& Topology() const
	{
		auto current = Read();
		if (!current.topology)
			throw std::runtime_error("No render graph is available");
		return *current.topology;
	}

	const CpuGraphNode& FindNode(const std::string& name) const
	{
		for (const auto& node : Topology().nodes)
			if (node->name == name)
				return *node;
		throw py::key_error("Unknown graph node '" + name + "'");
	}

	void RequireEditable() const
	{
		if (!Read().uid)
			throw py::value_error("Call neurus.render.graph.create() before editing the built-in graph");
	}
};

// Resolve every use against the current document. Its epoch changes on project
// new/open, including reopening files with the same pooled graph UID.
struct NodeProxy
{
	GraphProxy graph;
	std::string name;
	const CpuGraphNode* identity = nullptr;
	ScriptGraphView document;
	std::size_t generation;

	NodeProxy(const GraphProxy& g, const CpuGraphNode* node)
		: graph(g), name(node ? node->name : ""), identity(node),
		  document(g.Read()), generation(g.generations->Get(name, document.projectEpoch))
	{}

	void CheckDocument() const
	{
		const auto current = graph.Read();
		if (!current.topology || current.uid != document.uid || current.projectEpoch != document.projectEpoch)
			throw py::value_error("This graph handle belongs to a previous document");
	}

	const CpuGraphNode& Get() const
	{
		CheckDocument();
		const CpuGraphNode& node = graph.FindNode(name);
		if (identity != &node || generation != graph.generations->Get(name, document.projectEpoch))
			throw py::value_error("Stale graph node '" + name + "'");
		return node;
	}
};

enum class SocketKind { Input, Output, SceneOutput };

struct SocketProxy
{
	NodeProxy owner;
	std::string name;
	SocketKind kind;

	const CpuPort* Port() const
	{
		owner.CheckDocument();
		if (kind == SocketKind::SceneOutput)
			return nullptr;
		const CpuGraphNode& node = owner.Get();
		if (kind == SocketKind::Output)
		{
			for (const auto& socket : node.outputs)
				if (socket->name == name)
					return &socket->data;
		}
		else
		{
			for (const auto& socket : node.inputs)
				if (socket->name == name)
					return &socket->data;
		}
		throw py::key_error("Stale socket '" + node.name + "." + name + "'");
	}

	GraphEndpoint Endpoint() const
	{
		Port();
		return {owner.name, name};
	}

	std::optional<SocketProxy> Source() const
	{
		Port();
		if (kind == SocketKind::SceneOutput)
		{
			const auto& outputs = owner.graph.Topology().sceneOutputs;
			auto found = outputs.find(name);
			if (found != outputs.end())
			{
				const auto& endpoint = found->second;
				return SocketProxy{NodeProxy(owner.graph, &owner.graph.FindNode(endpoint.node)),
				                   endpoint.port, SocketKind::Output};
			}
		}
		else if (kind == SocketKind::Input)
		{
			for (const auto& input : owner.Get().inputs)
			{
				if (input->name == name && input->IsConnected())
					return SocketProxy{NodeProxy(owner.graph, input->incoming.from->owner),
					                   input->incoming.from->name, SocketKind::Output};
			}
		}
		return std::nullopt;
	}

	void SetTag(const GraphOutputTag& tag) const
	{
		owner.graph.RequireEditable();
		if (kind != SocketKind::Output)
			throw py::value_error("Resource tags belong to pass output sockets");
		EmitCheckedEvent(owner.graph, RenderGraphTagRequested{Endpoint(), tag});
	}
};

struct Edge
{
	std::string fromNode;
	std::string fromPort;
	std::string toNode;
	std::string toPort;
};

template<typename SocketList>
py::dict NodeSockets(const NodeProxy& node, const SocketList& sockets, SocketKind kind)
{
	py::dict result;
	for (const auto& socket : sockets)
		result[py::str(socket->name)] = py::cast(SocketProxy{node, socket->name, kind});
	return result;
}

void RegisterTags(py::module_& render)
{
	py::enum_<GraphFormat>(render, "GraphFormat")
		.value("none", GraphFormat::None).value("rgba16f", GraphFormat::Rgba16f)
		.value("rgba8_srgb", GraphFormat::Rgba8Srgb).value("rgba8_unorm", GraphFormat::Rgba8Unorm)
		.value("r8_unorm", GraphFormat::R8Unorm).value("r32_uint", GraphFormat::R32Uint)
		.value("d32_sfloat", GraphFormat::D32Sfloat);
	py::enum_<GraphUsage>(render, "GraphUsage")
		.value("logical", GraphUsage::Logical).value("color_sampled", GraphUsage::ColorSampled)
		.value("storage_sampled", GraphUsage::StorageSampled).value("depth_sampled", GraphUsage::DepthSampled);
	py::enum_<GraphExtent>(render, "GraphExtent")
		.value("render", GraphExtent::Render).value("shadow", GraphExtent::Shadow);
	py::enum_<GraphLayers>(render, "GraphLayers")
		.value("single", GraphLayers::Single).value("lights", GraphLayers::Lights);
	py::class_<GraphOutputTag>(render, "OutputTag")
		.def(py::init<GraphFormat, GraphUsage, GraphExtent, GraphLayers>(),
		     py::arg("format"), py::arg("usage"), py::arg("extent") = GraphExtent::Render,
		     py::arg("layers") = GraphLayers::Single)
		.def_readwrite("format", &GraphOutputTag::format).def_readwrite("usage", &GraphOutputTag::usage)
		.def_readwrite("extent", &GraphOutputTag::extent).def_readwrite("layers", &GraphOutputTag::layers);
}

} // namespace

void RegisterGraphBindings(py::module_& render)
{
	RegisterTags(render);
	py::class_<Edge>(render, "GraphEdge")
		.def_readonly("from_node", &Edge::fromNode).def_readonly("from_port", &Edge::fromPort)
		.def_readonly("to_node", &Edge::toNode).def_readonly("to_port", &Edge::toPort);
	py::class_<CpuGraphDiagnostic>(render, "GraphDiagnostic")
		.def_readonly("node", &CpuGraphDiagnostic::node)
		.def_readonly("port", &CpuGraphDiagnostic::port)
		.def_readonly("message", &CpuGraphDiagnostic::message);
	py::class_<SocketProxy>(render, "GraphSocket")
		.def_property_readonly("name", [](const SocketProxy& s) { return s.Endpoint().port; })
		.def_property_readonly("node", [](const SocketProxy& s) { return s.Endpoint().node; })
		.def_property_readonly("type", [](const SocketProxy& s) -> std::optional<std::string> {
			const auto* port = s.Port();
			return port ? std::optional(port->type) : std::nullopt;
		})
		.def_property_readonly("source", &SocketProxy::Source)
		.def_property("tag", [](const SocketProxy& s) -> std::optional<GraphOutputTag> {
			const auto* port = s.Port();
			return port ? port->tag : std::nullopt;
		}, &SocketProxy::SetTag);
	py::class_<NodeProxy>(render, "GraphNode")
		.def_property_readonly("name", [](const NodeProxy& n) { return n.Get().name; })
		.def_property_readonly("kind", [](const NodeProxy& n) { return n.Get().data.pass; })
		.def_property_readonly("inputs", [](const NodeProxy& n) { return NodeSockets(n, n.Get().inputs, SocketKind::Input); })
		.def_property_readonly("outputs", [](const NodeProxy& n) { return NodeSockets(n, n.Get().outputs, SocketKind::Output); })
		.def_property("output_tags", [](const NodeProxy& n) {
			std::map<std::string, GraphOutputTag> tags;
			for (const auto& output : n.Get().outputs)
				if (output->data.tag)
					tags.emplace(output->name, *output->data.tag);
			return tags;
		}, [](const NodeProxy& n, const std::map<std::string, GraphOutputTag>& tags) {
			for (const auto& [name, tag] : tags)
				SocketProxy{n, name, SocketKind::Output}.SetTag(tag);
		})
		.def_property("shader", [](const NodeProxy& n) -> std::optional<ShaderProxy> {
			const auto& shader = n.Get().data.shader;
			if (shader)
				return ShaderProxy(shader->GetObjectID(), n.graph.view);
			return std::nullopt;
		}, [](const NodeProxy& n, const std::optional<ShaderProxy>& shader) {
			n.graph.RequireEditable();
			if (shader)
				shader->Resolve();
			EmitCheckedEvent(n.graph, RenderGraphShaderRequested{n.Get().name, shader ? std::optional(shader->uid) : std::nullopt});
		})
		.def("create_shader", [](const NodeProxy& n) {
			n.graph.RequireEditable();
			int uid = 0;
			EmitCheckedEvent(n.graph, RenderGraphCreateShaderRequested{n.Get().name, &uid});
			return ShaderProxy(uid, n.graph.view);
		}, "Create this pass's editable shader from its primary built-in recipe.");
	py::class_<GraphProxy>(render, "RenderGraph")
		.def_property_readonly("uid", [](const GraphProxy& g) { return g.Read().uid; })
		.def_property_readonly("nodes", [](const GraphProxy& g) {
			py::dict nodes;
			for (const auto& node : g.Topology().nodes)
				nodes[py::str(node->name)] = py::cast(NodeProxy(g, node.get()));
			return nodes;
		})
		.def_property_readonly("edges", [](const GraphProxy& g) {
			std::vector<Edge> edges;
			for (const auto* link : g.Topology().Connections())
				edges.push_back({link->from->owner->name, link->from->name, link->to->owner->name, link->to->name});
			return edges;
		})
		.def_property_readonly("outputs", [](const GraphProxy& g) {
			g.Topology();
			py::dict outputs;
			for (const auto name : kSceneOutputNames)
				outputs[py::str(name)] = py::cast(SocketProxy{NodeProxy(g, nullptr), std::string(name), SocketKind::SceneOutput});
			return outputs;
		})
		.def("validate", [](const GraphProxy& g) {
			if (!g.view || !g.view->ctx || !g.view->ctx->graphDiagnostics)
				throw std::runtime_error("No render graph is available for validation");
			return g.view->ctx->graphDiagnostics();
		})
		.def("create", [](GraphProxy& g) -> GraphProxy& {
			EmitCheckedEvent(g, RenderGraphCreateRequested{});
			if (!g.Read().uid)
				throw std::runtime_error("No project graph was created");
			return g;
		}, py::return_value_policy::reference, "Create the editable project graph if needed and return it.")
		.def("add", [](const GraphProxy& g, const std::string& kind, const std::string& id) {
			g.RequireEditable();
			std::string created;
			EmitCheckedEvent(g, RenderGraphAddRequested{kind, id, &created});
			return NodeProxy(g, &g.FindNode(created));
		}, py::arg("kind"), py::arg("id") = "")
		.def("remove", [](const GraphProxy& g, const std::string& id) {
			g.RequireEditable();
			EmitCheckedEvent(g, RenderGraphRemoveRequested{id});
			g.generations->active.erase(id);
		}, py::arg("id"))
		.def("rename", [](const GraphProxy& g, const std::string& oldName, const std::string& newName) {
			g.RequireEditable();
			EmitCheckedEvent(g, RenderGraphRenameRequested{oldName, newName});
			if (oldName != newName)
				g.generations->active.erase(oldName);
			return NodeProxy(g, &g.FindNode(newName));
		}, py::arg("old"), py::arg("new"))
		.def("connect", [](const GraphProxy& g, const SocketProxy& source, const SocketProxy& dest) {
			g.RequireEditable();
			if (source.kind != SocketKind::Output || dest.kind == SocketKind::Output)
				throw py::value_error("Connect a pass output to a pass input or a scene output");
			EmitCheckedEvent(g, RenderGraphConnectRequested{source.Endpoint(), dest.Endpoint()});
		}, py::arg("source"), py::arg("destination"))
		.def("disconnect", [](const GraphProxy& g, const SocketProxy& dest) {
			g.RequireEditable();
			if (dest.kind == SocketKind::Output)
				throw py::value_error("Disconnect a pass input or a scene output");
			EmitCheckedEvent(g, RenderGraphDisconnectRequested{dest.Endpoint()});
		}, py::arg("destination"));
}

void InstallGraphProxy(py::module_& render, const ScriptView* view)
{
	render.attr("graph") = py::cast(GraphProxy{{0, view}, std::make_shared<NodeGenerations>()});
}

} // namespace neurus::script
