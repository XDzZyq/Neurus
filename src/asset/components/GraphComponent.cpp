#include "asset/components/GraphComponent.hpp"

#include "asset/ResourceManager.hpp"
#include "asset/data/RenderGraphData.hpp"

#include <nlohmann/json.hpp>

#include <limits>

namespace neurus::project
{
namespace
{
struct GraphSelection
{
	ResourceRef<RenderGraphData>& graph;
	const IResourceLookup& pool;
	std::vector<std::string>& diagnostics;

	template<class Archive>
	void serialize(Archive& ar)
	{
		const auto uid = archive::PoolRefId(ar, "uid", graph, pool);
		if constexpr (Archive::is_loading::value)
		{
			if (uid && !graph)
			{
				diagnostics.push_back("Missing render graph UID " + std::to_string(*uid));
				NEURUS_LOG("[GraphComponent] " << diagnostics.back());
			}
		}
	}
};
} // namespace

GraphComponent::GraphComponent(ResourceRef<RenderGraphData>& graph, ResourceManager& resources,
                               std::vector<std::string>& diagnostics)
	: m_graph(graph), m_resources(resources), m_diagnostics(diagnostics)
{
}

void GraphComponent::Save(cereal::JSONOutputArchive& ar) const
{
	if (!m_graph) return;
	GraphSelection selection{m_graph, m_resources, m_diagnostics};
	ar(cereal::make_nvp("proj_render_graph", selection));
}

void GraphComponent::Load(cereal::JSONInputArchive& ar)
{
	m_graph.reset();
	GraphSelection selection{m_graph, m_resources, m_diagnostics};
	archive::OptionalField(ar, cereal::make_nvp("proj_render_graph", selection));
}

void GraphComponent::LoadJson(const std::string& object)
{
	m_graph.reset();
	const auto document = nlohmann::json::parse(object, nullptr, false);
	const auto graph = document.find("proj_render_graph");
	if (graph == document.end() || graph->is_null()) return;
	const auto uid = graph->find("uid");
	if (uid == graph->end() || !uid->is_number_integer() || *uid < 0 || *uid >= std::numeric_limits<int>::max())
	{
		NEURUS_LOG("[GraphComponent] Invalid graph selection");
		return;
	}
	Serializable::LoadJson(object);
}
} // namespace neurus::project
