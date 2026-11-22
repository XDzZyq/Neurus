#include "editor/RenderManager.hpp"

#include "shader/BuiltInRenderData.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "core/Log.hpp"
#include "core/ResourceManager.hpp"
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

void RenderManager::ResetGraph()
{
	m_graph.reset();
	m_pending.reset();
	m_problems.clear();
	Publish();
}

void RenderManager::ValidateGraph()
{
	if (!m_graph)
		return;
	for (const auto& node : m_graph->nodes)
	{
		if (node->data.shader && !node->data.shader->IsValid())
			node->data.shader.reset();
	}
	for (const auto& diagnostic : ValidateCpuGraph(*m_graph, m_renderDefaults->Ports()))
		m_problems.push_back(diagnostic.node + "." + diagnostic.port + ": " + diagnostic.message);
	if (!m_problems.empty())
	{
		for (const auto& problem : m_problems)
			NEURUS_LOG("[RenderManager] " << problem << " - using built-in graph");
		m_graph.reset();
	}
}

void RenderManager::Restore(const project::RenderGraphRecord& record)
{
	m_problems.clear();
	m_graph = std::make_shared<RenderGraphData>(
		project::FromRecord(record, m_renderDefaults->Ports(), m_problems, &m_resources));
	ValidateGraph();
	Publish();
}

void RenderManager::SetGraph(RenderGraphData graph)
{
	m_problems.clear();
	m_graph = std::make_shared<RenderGraphData>(std::move(graph));
	ValidateGraph();
	Publish();
}

void RenderManager::Publish()
{
	if (!m_sink)
		return;
	std::shared_ptr<const RenderGraphData> published;
	if (m_graph)
	{
		auto graph = std::make_shared<RenderGraphData>(CloneGraph(*m_graph));
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
