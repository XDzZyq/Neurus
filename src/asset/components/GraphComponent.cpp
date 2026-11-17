#include "asset/components/GraphComponent.hpp"

#include "core/Log.hpp"
#include "asset/data/render_graph/RenderGraphSpec.hpp"
#include "core/Serialize.hpp"

#include <cereal/cereal.hpp>

#include <utility>

namespace neurus::project
{

GraphComponent::GraphComponent(RenderGraphSpec& spec) : m_spec(&spec) {}

void GraphComponent::Save(cereal::JSONOutputArchive& ar) const
{
	ar(cereal::make_nvp("proj_render_graph", *m_spec));
}

void GraphComponent::Load(cereal::JSONInputArchive& ar)
{
	// Read into a default document: every node and edge is optional at the
	// field level, and an absent node means "the built-in default", never the
	// previous session's graph.
	RenderGraphSpec fresh = MakeDefaultRenderGraphSpec();
	try
	{
		if (!archive::LoadFresh(ar, "proj_render_graph", fresh))
		{
			NEURUS_LOG("GraphComponent::Load: no proj_render_graph node"
			           " - defaulted (old project format).");
			fresh = MakeDefaultRenderGraphSpec();
		}
	}
	catch (const cereal::Exception& e)
	{
		NEURUS_ERR("GraphComponent::Load: " << e.what()
		           << " - proj_render_graph defaulted (corrupt node).");
		fresh = MakeDefaultRenderGraphSpec();
	}

	*m_spec = std::move(fresh);
}

} // namespace neurus::project
