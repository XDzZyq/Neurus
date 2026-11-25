/**
 * @file RenderGraphData.hpp
 * @brief CPU scene graph used for project resources, built-in defaults and publication.
 */
#pragma once

#include "asset/data/render_graph/GraphEndpoint.hpp"
#include "asset/data/render_graph/PassPorts.hpp"
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
 * @brief Socket payload of the CPU graph: one shader port.
 *
 * Mirrors @ref PassPort, plus the resource declaration an output owns. The
 * declaration is user data: a shader names a GLSL type, never an image format.
 * An input never carries one; @c tag is only meaningful for an output.
 */
struct CpuPort
{
	std::string    name;
	PortDirection  direction = PortDirection::Input;
	std::string    type;          ///< "sampler2D", "image2D", "vec4", "uint"
	uint32_t       binding = 0;   ///< set-0 binding; 0 for a fragment output
	uint32_t       location = 0;  ///< fragment output location; 0 otherwise
	std::optional<GraphOutputTag> tag;   ///< outputs only

	bool operator==(const CpuPort&) const = default;
};

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
			auto record = std::make_shared<project::RenderGraphRecord>();
			ar(cereal::make_nvp("topology", *record));
			ArchiveResourceRefs::AfterLoad(ar, GetObjectID(),
				[this, record = std::move(record)](const IResourceLookup& pool, const ResourceReadServices* services) {
					return project::RestoreGraph(*this, *record, pool, services);
				});
		}
		else
		{
			ar(cereal::make_nvp("topology", project::ToRecord(*this)));
		}
	}

};

std::optional<GraphOutputTag> BuiltInOutputTag(std::string_view kind, std::string_view port);
PassPorts EffectivePassPorts(const CpuNode& node, const IPortSource& catalog);
PassPorts WithShaderPortNames(const PassPorts& builtin, const PassPorts& shaderPorts);
void ReconcilePassPorts(RenderGraphData& graph, RenderGraphData::NodeType& node, const PassPorts& ports);

} // namespace neurus
