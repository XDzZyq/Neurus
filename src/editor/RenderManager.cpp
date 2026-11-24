#include "editor/RenderManager.hpp"

#include "shader/BuiltInRenderData.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "core/Log.hpp"
#include "asset/ResourceManager.hpp"
#include "scene/Mesh.hpp"
#include "render_api/RenderRequestSink.hpp"
#include "render_api/RenderRequests.hpp"

#include <utility>
#include <unordered_set>

namespace neurus {

RenderManager::RenderManager(ResourceManager& resources, RenderRequestSink* sink,
                             std::shared_ptr<const BuiltInRenderData> renderDefaults)
	: m_resources(resources)
	, m_sink(sink)
	, m_renderDefaults(renderDefaults ? std::move(renderDefaults) : std::make_shared<const BuiltInRenderData>())
{
}
RenderManager::~RenderManager() = default;

RenderConfig& RenderManager::EditConfig()
{
	if (!m_config)
		m_config = std::make_shared<RenderConfig>();
	return *m_config;
}

const RenderGraphData& RenderManager::Graph() const
{
	return m_graph ? *m_graph : *m_renderDefaults->Graph();
}

void RenderManager::ResetConfig()
{
	m_config.reset();
}

void RenderManager::ResetGraph(bool publish)
{
	m_graph.reset();
	m_problems.clear();
	if (publish)
		Publish();
}

namespace {

void CheckTopology(RenderGraphData& graph, const IPortSource& ports, std::vector<std::string>& problems)
{
	for (const auto& node : graph.nodes)
	{
		if (node->data.shader && !node->data.shader->IsValid())
			node->data.shader = nullptr;
	}
	for (const auto& diagnostic : ValidateCpuGraph(graph, ports))
		problems.push_back(diagnostic.node + "." + diagnostic.port + ": " + diagnostic.message);
}

} // namespace

std::vector<CpuGraphDiagnostic> RenderManager::Diagnostics() const
{
	const auto& graph = Graph();
	auto diagnostics = ValidateCpuGraph(graph, m_renderDefaults->Ports());
	for (const auto& node : graph.nodes)
	{
		if (!node->data.shader)
			continue;
		try
		{
			m_renderDefaults->ValidatePassShader(node->data.pass, *node->data.shader);
		}
		catch (const std::exception& error)
		{
			diagnostics.push_back({node->name, {}, error.what()});
		}
	}
	return diagnostics;
}

std::shared_ptr<RenderGraphData> RenderManager::CreateGraph(bool publish)
{
	if (!m_graph)
		RestoreCreatedGraph(std::make_shared<RenderGraphData>(), publish);
	return m_graph.shared();
}

void RenderManager::RemoveCreatedGraph(const std::shared_ptr<RenderGraphData>& graph, bool publish)
{
	if (graph && m_graph.get() == graph.get())
	{
		m_resources.Remove(graph->GetObjectID());
		ResetGraph(publish);
	}
}

void RenderManager::RestoreCreatedGraph(const std::shared_ptr<RenderGraphData>& graph, bool publish)
{
	if (!graph || m_graph)
		return;
	// A creation restored from redo history carries only its original UID.
	if (graph->nodes.empty())
		graph->SwapContents(*m_renderDefaults->Graph()->Clone());
	m_resources.Register(graph);
	m_graph = graph;
	m_problems.clear();
	if (publish)
		Publish();
}

bool RenderManager::SetGraph(std::unique_ptr<RenderGraphData> graph)
{
	if (!graph)
	{
		ResetGraph();
		return true;
	}
	m_problems.clear();
	CheckTopology(*graph, m_renderDefaults->Ports(), m_problems);
	if (!m_problems.empty())
	{
		for (const auto& problem : m_problems)
			NEURUS_LOG("[RenderManager] Rejected graph edit: " << problem);
		return false;
	}
	if (m_graph)
		m_graph->SwapContents(*graph);
	else
	{
		m_graph = std::shared_ptr<RenderGraphData>(std::move(graph));
		m_resources.Register(m_graph.shared());
	}
	Publish();
	return true;
}

void RenderManager::RegisterShader(const std::shared_ptr<ShaderData>& shader)
{
	const int uid = m_resources.Register(shader);
	shader->SetName(m_resources.SetName(uid, shader->GetName()));
}

std::shared_ptr<ShaderData> RenderManager::RemoveUnusedShader(int uid)
{
	bool used = false;
	m_resources.ForEach<RenderGraphData>([&](const std::shared_ptr<RenderGraphData>& graph) {
		for (const auto& node : graph->nodes)
			if (node->data.shader && node->data.shader.id() == uid)
				used = true;
	});
	m_resources.ForEach<Mesh>([&](const std::shared_ptr<Mesh>& mesh) {
		if (mesh->o_shader && mesh->o_shader->GetObjectID() == uid)
			used = true;
	});
	if (used)
		return nullptr;
	auto shader = m_resources.Get<ShaderData>(uid);
	if (shader)
		m_resources.Remove(uid);
	return shader;
}

void RenderManager::UploadShader(const ShaderData& shader)
{
	if (m_sink)
		m_sink->Post(MakeUploadShaderRequest(shader));
}

void RenderManager::Publish()
{
	if (!m_sink)
		return;
	std::shared_ptr<const RenderGraphData> published;
	if (m_graph)
	{
		auto graph = m_graph->Clone();
		std::unordered_set<int> uploaded;
		for (const auto& node : graph->nodes)
		{
			if (const auto& shader = node->data.shader;
			    shader && uploaded.insert(shader->GetObjectID()).second)
				m_sink->Post(MakeUploadShaderRequest(*shader));
		}
		published = std::move(graph);
	}
	m_sink->Post(CompileRenderGraphRequest{std::move(published)});
}

bool RenderManager::ReferencesShader(const int uid) const
{
	if (!m_graph)
		return false;
	for (const auto& node : m_graph->nodes)
	{
		if (node->data.shader && node->data.shader.id() == uid)
			return true;
	}
	return false;
}

} // namespace neurus
