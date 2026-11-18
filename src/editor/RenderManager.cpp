/**
 * @file RenderManager.cpp
 * @brief Document lifecycle, shader seeding and publication.
 */

#include "RenderManager.hpp"

#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "asset/data/render_graph/PassKinds.hpp"
#include "asset/data/render_graph/PassShaders.hpp"
#include "asset/data/ShaderData.hpp"

#include "render/RenderFrameSnapshot.hpp"
#include "render/requests/RenderRequestSink.hpp"
#include "render/requests/RenderRequests.hpp"
#include "render/shaders/ComputeShader.hpp"
#include "render/shaders/RenderShader.hpp"
#include "render/shaders/ShaderLibrary.hpp"

#include "core/Log.hpp"
#include "core/ResourceManager.hpp"

#include <exception>
#include <span>
#include <unordered_map>
#include <utility>

namespace neurus {

namespace {

/**
 * @brief Creates the default shader for one source of a built-in pass kind.
 *
 * @param source  Shader source row.
 * @return Parsed shader, or nullptr when loading failed.
 */
std::shared_ptr<ShaderData> LoadPassKindShader(const PassShaderSource& source)
{
	if (!source.computePath.empty())
	{
		return ShaderLibrary::LoadComputeShader(
			std::string(source.name), std::string(source.computePath));
	}

	return ShaderLibrary::LoadRenderShader(
		std::string(source.name), std::string(source.vertexPath),
		std::string(source.fragmentPath));
}

} // anonymous namespace

RenderManager::RenderManager(ResourceManager& resources, RenderRequestSink* sink)
	: m_resources(resources)
	, m_sink(sink)
{
}

RenderManager::~RenderManager() = default;

void RenderManager::SeedShaders()
{
	m_passShaders.reset();
	m_passShaders = std::make_unique<PassShaders>(m_resources, &LoadPassKindShader, m_problems);
}

void RenderManager::AssignNodeShaders()
{
	for (const auto& node : m_graph.nodes)
	{
		if (node->data.shaderUid != 0 && m_resources.Contains(node->data.shaderUid))
			continue;
		node->data.shaderUid = m_passShaders->ShaderUid(node->data.pass);
	}
}

void RenderManager::Reset()
{
	m_problems.clear();
	m_config = RenderConfig{};
	SeedShaders();

	m_graph = RenderGraphData::Default(*m_passShaders);
	AssignNodeShaders();
	Publish();
}

void RenderManager::Restore(const project::RenderGraphRecord& record)
{
	m_problems.clear();
	SeedShaders();

	std::vector<std::string> loadProblems;
	m_graph = project::FromRecord(record, *m_passShaders, loadProblems);
	m_problems.insert(m_problems.end(), loadProblems.begin(), loadProblems.end());

	AssignNodeShaders();
	Publish();
}

void RenderManager::SetGraph(RenderGraphData graph)
{
	if (!m_passShaders)
		SeedShaders();

	m_graph = std::move(graph);
	AssignNodeShaders();
	Publish();
}

void RenderManager::Publish()
{
	if (!m_sink)
		return;

	for (const CpuGraphDiagnostic& diagnostic : ValidateCpuGraph(m_graph, *m_passShaders))
	{
		NEURUS_ERR("[RenderManager] render graph: " << diagnostic.node << "."
		           << diagnostic.port << ": " << diagnostic.message);
	}

	// Snapshot every shader the document names, including the second shader of
	// the shadow passes, so the renderer never reads this pool. A shader that
	// fails to snapshot is reported and left out; the renderer then rejects the
	// node by name instead of drawing against a stale shader.
	std::vector<ShaderSnapshot> snapshots;
	std::unordered_map<int, bool> seen;
	for (const auto& node : m_graph.nodes)
	{
		const PassKindInfo* kind = FindPassKind(node->data.pass);
		const std::span<const uint32_t> optional =
			kind ? kind->optionalBindings : std::span<const uint32_t>{};

		for (const int uid : m_passShaders->ShaderUids(node->data.pass))
		{
			if (!seen.emplace(uid, true).second)
				continue;

			const auto shader = m_resources.Get<ShaderData>(uid);
			if (!shader)
				continue;

			try
			{
				snapshots.push_back(MakeShaderSnapshot(*shader, optional));
			}
			catch (const std::exception& error)
			{
				NEURUS_ERR("[RenderManager] cannot snapshot shader " << uid
				           << " for node '" << node->name << "': " << error.what());
			}
		}
	}

	m_sink->Post(CompileRenderGraphRequest{
		std::make_shared<const RenderGraphData>(CloneGraph(m_graph)),
		std::move(snapshots)});
}

} // namespace neurus
