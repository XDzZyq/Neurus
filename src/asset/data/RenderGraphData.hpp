/**
 * @file RenderGraphData.hpp
 * @brief CPU scene graph used for project resources, built-in defaults and publication.
 */
#pragma once

#include "asset/data/render_graph/GraphEndpoint.hpp"
#include "asset/data/render_graph/CpuPorts.hpp"
#include "asset/data/render_graph/RenderGraphRecord.hpp"
#include "core/Graph.hpp"
#include "core/UID.hpp"

#include <cereal/types/base_class.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace neurus {

inline constexpr std::array<std::string_view, 3> kSceneOutputNames = {"color", "depth", "object_id"};

class IPortSource;
class ShaderData;

/**
 * @brief Node payload of the CPU graph: one pass instance plus its editor state.
 *
 * The node's document id is `Graph::Node::name`, which the graph editor and the
 * script API use. A pass kind names a built-in factory entry; @c shader owns the
 * selected CPU shader (null selects the built-in), and the position is layout that
 * the renderer never reads.
 */
struct CpuNode
{
	std::string pass;
	PassShaderRef shader;
	float       x = 0.0f;
	float       y = 0.0f;

	bool operator==(const CpuNode&) const = default;
};

/** @brief CPU scene graph for editing, persistence and renderer publication. */
class RenderGraphData : public UID, public Graph<CpuPort, CpuNode>
{
public:
	using GraphT = Graph<CpuPort, CpuNode>;

	RenderGraphData() = default;
	/// Builds the built-in scene graph from the declared pass ports.
	explicit RenderGraphData(const IPortSource& ports);

	RenderGraphData(const RenderGraphData&) = delete;
	RenderGraphData& operator=(const RenderGraphData&) = delete;
	RenderGraphData(RenderGraphData&&) = delete;
	RenderGraphData& operator=(RenderGraphData&&) = delete;

	/// Copies nodes, sockets and connections into an unpooled document with its own UID.
	std::unique_ptr<RenderGraphData> Clone() const;

	/// Exchanges document contents while preserving both object identities.
	void SwapContents(RenderGraphData& other) noexcept;

	std::map<std::string, GraphEndpoint> sceneOutputs;
	std::set<std::string> ActiveNodes() const;

	/**
	 * @brief Stable hash of the topology-affecting content.
	 *
	 * Covers node names, pass kinds, shader identities, output tags and every
	 * connection. Editor layout (`x`, `y`) is excluded: moving a node must not
	 * recompile the graph.
	 */
	std::uint64_t Hash() const;

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<UID>(this));
		if constexpr (Archive::is_loading::value)
		{
			project::RenderGraphRecord record;
			ar(cereal::make_nvp("topology", record));
			const auto* input = dynamic_cast<const ResourceInputArchive*>(&ar);
			if (!input)
				throw cereal::Exception("Reading a render graph requires a resource pool");
			if (!project::RestoreGraph(*this, record, input->Pool()))
				throw cereal::Exception("Could not restore render graph topology");
		}
		else
		{
			ar(cereal::make_nvp("topology", project::ToRecord(*this)));
		}
	}

};

std::optional<GraphOutputTag> BuiltInOutputTag(std::string_view kind, std::string_view port);
CpuPorts EffectivePassPorts(const CpuNode& node, const IPortSource& catalog);
CpuPorts WithShaderPortNames(const CpuPorts& builtin, const CpuPorts& shaderPorts);
void ReconcilePassPorts(RenderGraphData& graph, RenderGraphData::NodeType& node, const CpuPorts& ports);

} // namespace neurus
