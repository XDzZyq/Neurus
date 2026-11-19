#include "editor/RenderManager.hpp"

#include "shader/BuiltInRenderData.hpp"
#include "shader/ShaderSnapshotBuilder.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "asset/data/render_graph/PassKinds.hpp"
#include "core/Log.hpp"
#include "core/ResourceManager.hpp"
#include "render_api/RenderRequestSink.hpp"
#include "render_api/RenderRequests.hpp"

#include <exception>
#include <unordered_set>
#include <utility>

namespace neurus {

RenderManager::RenderManager(ResourceManager& resources, RenderRequestSink* sink)
	: m_resources(resources), m_sink(sink)
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
	return m_graph ? *m_graph : *BuiltInRenderData::Get().Graph();
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
		if (node->data.shaderUid == 0)
			continue;
		const auto shader = m_resources.Get<ShaderData>(node->data.shaderUid);
		if (!shader || !shader->IsValid())
			node->data.shaderUid = 0;
	}
	for (const auto& diagnostic : ValidateCpuGraph(*m_graph, BuiltInRenderData::Get().Ports()))
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
		project::FromRecord(record, BuiltInRenderData::Get().Ports(), m_problems));
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
	std::vector<ShaderSnapshot> snapshots;
	std::unordered_set<int> seen;
	if (m_graph)
	{
		for (const auto& node : m_graph->nodes)
		{
			const PassKindInfo* kind = FindPassKind(node->data.pass);
			if (!kind)
				continue;
			std::vector<int> shaderIds{node->data.shaderUid};
			// Secondary built-in variants can be explicitly stored by source name.
			for (const PassShaderSource& source : kind->shaders)
			{
				m_resources.ForEachUID([&](const std::shared_ptr<UID>& value)
				{
					const auto shader = std::dynamic_pointer_cast<ShaderData>(value);
					if (shader && shader->GetName() == source.name && shader->IsValid())
						shaderIds.push_back(shader->GetObjectID());
				});
			}
			for (const int uid : shaderIds)
			{
				if (!uid || !seen.insert(uid).second)
					continue;
				const auto shader = m_resources.Get<ShaderData>(uid);
				if (!shader || !shader->IsValid())
					continue;
				try
				{
					snapshots.push_back(MakeShaderSnapshot(*shader, kind->optionalBindings));
				}
				catch (const std::exception& error)
				{
					NEURUS_LOG("[RenderManager] Cannot snapshot shader " << uid << ": " << error.what());
				}
			}
		}
	}
	m_sink->Post(CompileRenderGraphRequest{
		m_graph ? std::make_shared<const RenderGraphData>(CloneGraph(*m_graph)) : nullptr,
		std::move(snapshots)});
}

} // namespace neurus
