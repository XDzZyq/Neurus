#include "editor/RenderManager.hpp"

#include "shader/BuiltInRenderData.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "core/Log.hpp"
#include "core/ResourceManager.hpp"
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

const RenderGraphTopology& RenderManager::Graph() const
{
	return m_graph ? *m_graph : *m_renderDefaults->Graph();
}

void RenderManager::ResetConfig()
{
	m_config.reset();
}

void RenderManager::BeginGraphLoad()
{
	m_graph.reset();
	m_pending.reset();
	m_problems.clear();
}

void RenderManager::ResetGraph()
{
	BeginGraphLoad();
	Publish();
}

namespace {

void CheckTopology(RenderGraphTopology& graph, const IPortSource& ports, std::vector<std::string>& problems)
{
	for (const auto& node : graph.nodes)
	{
		if (node->data.shader && !node->data.shader->IsValid())
			node->data.shader.reset();
	}
	for (const auto& diagnostic : ValidateCpuGraph(graph, ports))
		problems.push_back(diagnostic.node + "." + diagnostic.port + ": " + diagnostic.message);
}

} // namespace

void RenderManager::FinishGraphLoad()
{
	auto selection = std::exchange(m_pending, std::nullopt);
	m_problems.clear();
	m_graph.reset();

	const int* selectedUid = selection ? std::get_if<int>(&*selection) : nullptr;
	if (selectedUid)
	{
		m_graph = m_resources.Get<RenderGraphData>(*selectedUid);
		if (!m_graph)
			m_problems.push_back("Missing render graph UID " + std::to_string(*selectedUid));
	}

	std::vector<std::string> warnings;
	const auto acceptsShader = [&](const std::string& kind, const ShaderData& shader) {
		try
		{
			m_renderDefaults->ValidatePassShader(kind, shader);
			return true;
		}
		catch (const std::exception& error)
		{
			warnings.push_back(kind + " shader UID " + std::to_string(shader.GetObjectID())
			                   + " uses its built-in fallback: " + error.what());
			return false;
		}
	};

	// Pool reads cannot resolve connections until shaders and their port contracts are available.
	std::vector<int> invalid;
	m_resources.ForEach<RenderGraphData>([&](const std::shared_ptr<RenderGraphData>& graph)
	{
		std::vector<std::string> problems;
		if (graph->Resolve(m_renderDefaults->Ports(), m_resources, problems, acceptsShader))
			CheckTopology(*graph, m_renderDefaults->Ports(), problems);
		if (problems.empty())
			return;
		invalid.push_back(graph->GetObjectID());
		if (graph == m_graph)
		{
			m_problems = std::move(problems);
			m_graph.reset();
		}
		else
		{
			for (const auto& problem : problems)
				NEURUS_LOG("[RenderManager] Ignoring graph " << graph->GetObjectID() << ": " << problem);
		}
	});
	for (const int uid : invalid)
		m_resources.Remove(uid);

	if (selection && !selectedUid)
	{
		auto topology = project::FromRecord(std::get<project::RenderGraphRecord>(*selection),
		                                    m_renderDefaults->Ports(), m_problems, &m_resources, acceptsShader);
		CheckTopology(topology, m_renderDefaults->Ports(), m_problems);
		if (m_problems.empty())
			m_graph = m_resources.Load<RenderGraphData>(std::move(topology));
	}
	for (const auto& problem : m_problems)
		NEURUS_LOG("[RenderManager] " << problem << " - using built-in graph");
	for (const auto& warning : warnings)
		NEURUS_LOG("[RenderManager] " << warning);
	m_problems.insert(m_problems.end(), warnings.begin(), warnings.end());
	Publish();
}

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
	return m_graph;
}

void RenderManager::RemoveCreatedGraph(const std::shared_ptr<RenderGraphData>& graph, bool publish)
{
	if (graph && m_graph == graph)
	{
		m_resources.Remove(graph->GetObjectID());
		BeginGraphLoad();
		if (publish)
			Publish();
	}
}

void RenderManager::RestoreCreatedGraph(const std::shared_ptr<RenderGraphData>& graph, bool publish)
{
	if (!graph || m_graph)
		return;
	// A creation restored from redo history carries only its original UID.
	if (graph->nodes.empty())
		static_cast<RenderGraphTopology&>(*graph) = CloneGraph(*m_renderDefaults->Graph());
	m_resources.Register(graph);
	m_graph = graph;
	m_problems.clear();
	if (publish)
		Publish();
}

bool RenderManager::SetGraph(RenderGraphTopology graph)
{
	m_problems.clear();
	CheckTopology(graph, m_renderDefaults->Ports(), m_problems);
	if (!m_problems.empty())
	{
		for (const auto& problem : m_problems)
			NEURUS_LOG("[RenderManager] Rejected graph edit: " << problem);
		return false;
	}
	if (m_graph)
		static_cast<RenderGraphTopology&>(*m_graph) = std::move(graph);
	else
		m_graph = m_resources.Load<RenderGraphData>(std::move(graph));
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
			if (node->data.shader && node->data.shader->GetObjectID() == uid)
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
	std::shared_ptr<const RenderGraphTopology> published;
	if (m_graph)
	{
		auto graph = std::make_shared<RenderGraphTopology>(CloneGraph(Graph()));
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
		if (node->data.shader && node->data.shader->GetObjectID() == uid)
			return true;
	}
	return false;
}

} // namespace neurus
