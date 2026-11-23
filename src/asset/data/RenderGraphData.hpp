/**
 * @file RenderGraphData.hpp
 * @brief A pooled scene render graph and its saved topology.
 */
#pragma once

#include "asset/data/render_graph/RenderGraphRecord.hpp"
#include "asset/data/render_graph/RenderGraphTopology.hpp"
#include "core/UID.hpp"

#include <cereal/types/base_class.hpp>

#include <optional>
#include <string>
#include <vector>

namespace neurus {

class IPassShaderCatalog;

/** @brief An authored scene graph with a stable pool identity. */
class RenderGraphData : public UID, public RenderGraphTopology
{
public:
	RenderGraphData() = default;
	explicit RenderGraphData(RenderGraphTopology topology);
	explicit RenderGraphData(project::RenderGraphRecord record);

	/// Restores sockets and shader references once the entire pool is available.
	bool Resolve(const IPassShaderCatalog& catalog, const IResourceLookup& resources,
	             std::vector<std::string>& diagnostics);

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<UID>(this));
		if constexpr (Archive::is_loading::value)
		{
			ar(cereal::make_nvp("topology", m_pending.emplace()));
		}
		else if (m_pending)
		{
			ar(cereal::make_nvp("topology", *m_pending));
		}
		else
		{
			ar(cereal::make_nvp("topology", project::ToRecord(*this)));
		}
	}

private:
	std::optional<project::RenderGraphRecord> m_pending;
};

} // namespace neurus
