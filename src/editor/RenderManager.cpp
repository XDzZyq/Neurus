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
	m_problems->clear();
	if (publish)
		Publish();
}

std::vector<CpuGraphDiagnostic> RenderManager::Diagnostics() const
{
	return ValidateCpuGraph(Graph(), *m_renderDefaults);
}

std::shared_ptr<RenderGraphData> RenderManager::CreateGraph(bool publish)
{
	if (!m_graph)
	{
		auto graph = m_renderDefaults->Graph()->Clone();
		auto pooled = std::shared_ptr<RenderGraphData>(std::move(graph));
		m_resources.Register(pooled);
		m_graph = pooled;
		m_problems->clear();
		if (publish)
			Publish();
	}
	return m_graph.shared();
}

bool RenderManager::SetGraph(std::unique_ptr<RenderGraphData> graph)
{
	if (!graph)
	{
		ResetGraph();
		return true;
	}
	m_problems->clear();
	for (const auto& diagnostic : ValidateCpuGraph(*graph, *m_renderDefaults))
		m_problems->push_back(diagnostic.node + (diagnostic.port.empty() ? "" : "." + diagnostic.port)
		                      + ": " + diagnostic.message);
	if (!m_problems->empty())
	{
		for (const auto& problem : *m_problems)
			NEURUS_LOG("[RenderManager] Rejected graph edit: " << problem);
		return false;
	}
	if (m_graph)
		m_graph->SwapContents(*graph);
	else
	{
		auto pooled = std::shared_ptr<RenderGraphData>(std::move(graph));
		m_resources.Register(pooled);
		m_graph = std::move(pooled);
	}
	Publish();
	return true;
}

void RenderManager::RegisterShader(const std::shared_ptr<ShaderData>& shader)
{
	const int uid = m_resources.Register(shader);
	shader->SetName(m_resources.SetName(uid, shader->GetName()));
}

void RenderManager::DiscardCreation(int uid)
{
	if (m_resources.Get<RenderGraphData>(uid))
	{
		m_resources.Remove(uid);
		return;
	}
	// Only live objects could have acquired this new shader during the failed edit.
	for (const int id : m_resources.RegisteredIds())
	{
		const auto object = m_resources.GetLive(id);
		if (const auto graph = std::dynamic_pointer_cast<RenderGraphData>(object))
		{
			for (const auto& node : graph->nodes)
				if (node->data.shader && node->data.shader.id() == uid)
					return;
		}
		else if (const auto mesh = std::dynamic_pointer_cast<Mesh>(object))
		{
			if (mesh->o_shader && mesh->o_shader.id() == uid)
				return;
		}
	}
	m_resources.Remove(uid);
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
