#include "asset/components/GraphComponent.hpp"

#include "asset/data/RenderGraphData.hpp"
#include "core/Log.hpp"
#include "core/Serialize.hpp"

#include <cereal/cereal.hpp>

#include <optional>
#include <utility>

namespace neurus::project
{

GraphComponent::GraphComponent(std::optional<RenderGraphRecord>& pending,
                               const RenderGraphData& graph)
	: m_pending(&pending)
	, m_graph(&graph)
{
}

void GraphComponent::Save(cereal::JSONOutputArchive& ar) const
{
	const RenderGraphRecord record = m_graph->ToRecord();
	ar(cereal::make_nvp("proj_render_graph", record));
}

void GraphComponent::Load(cereal::JSONInputArchive& ar)
{
	// Every field is optional at the archive level; an absent key means "no
	// saved document", never the previous session's graph.
	RenderGraphRecord loaded;
	try
	{
		if (!archive::LoadFresh(ar, "proj_render_graph", loaded))
		{
			NEURUS_LOG("GraphComponent::Load: no proj_render_graph node"
			           " - the Editor will build the default graph.");
			m_pending->reset();
			return;
		}
	}
	catch (const cereal::Exception& e)
	{
		NEURUS_ERR("GraphComponent::Load: " << e.what()
		           << " - proj_render_graph defaulted (corrupt node).");
		m_pending->reset();
		return;
	}

	*m_pending = std::move(loaded);
}

} // namespace neurus::project
