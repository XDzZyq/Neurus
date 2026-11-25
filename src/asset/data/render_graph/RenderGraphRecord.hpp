/** @file RenderGraphRecord.hpp
 *  @brief Portable node and edge records for a pooled graph.
 */
#pragma once

#include "asset/data/render_graph/GraphOutputTag.hpp"
#include "asset/ResourceRef.hpp"
#include "asset/data/ShaderData.hpp"
#include "core/Serialize.hpp"

#include <cereal/archives/json.hpp>
#include <cereal/types/optional.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neurus
{
class IPassShaderCatalog;
using PassShaderRef = ResourceRef<const ShaderData>;
class IPortSource;
class IResourceLookup;
class RenderGraphData;
template<class, class> class Node;
struct CpuPort;
struct CpuNode;

/// Stores the image declaration on each serialized output socket.
template<class Archive>
void serialize(Archive& ar, GraphOutputTag& tag)
{
	auto field = [&]<class Enum>(const char* name, Enum& value, Enum last) {
		// Check the archive value before narrowing it to the enum's byte.
		auto raw = static_cast<std::uint32_t>(value);
		ar(cereal::make_nvp(name, raw));
		if constexpr (Archive::is_loading::value)
		{
			if (raw > static_cast<std::uint32_t>(last))
				throw cereal::Exception(std::string("Invalid graph output tag field: ") + name);
			value = static_cast<Enum>(raw);
		}
	};
	field("format", tag.format, GraphFormat::D32Sfloat);
	field("usage", tag.usage, GraphUsage::DepthSampled);
	field("extent", tag.extent, GraphExtent::Shadow);
	field("layers", tag.layers, GraphLayers::Lights);
}
}

namespace neurus::project
{

/// These optional scalars may be absent. A present field with the wrong JSON type rejects the graph.
template<class T>
bool ReadOptionalGraphField(cereal::JSONInputArchive& ar, const char* name, T& value)
{
	try
	{
		ar(cereal::make_nvp(name, value));
		return true;
	}
	catch (const cereal::RapidJSONException&) { throw; }
	catch (const cereal::Exception&) { return false; }
}

/**
 * @brief One saved output socket.
 */
struct RenderGraphPortRecord
{
	std::string                   name;
	std::string                   type;
	std::uint32_t                 binding = 0;
	std::uint32_t                 location = 0;
	std::optional<GraphOutputTag> tag;

	bool operator==(const RenderGraphPortRecord&) const = default;

	/**
	 * @brief Serializes the port.
	 * @tparam Archive Cereal archive type.
	 * @param ar Archive to read or write.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("name", name),
		   cereal::make_nvp("type", type),
		   cereal::make_nvp("binding", binding),
		   cereal::make_nvp("location", location),
		   cereal::make_nvp("tag", tag));
	}
};

/**
 * @brief One saved node: its document id, pass, shader, layout and outputs.
 */
struct RenderGraphNodeRecord
{
	std::string                        name;
	std::string                        pass;
	std::optional<int>                 shaderUid;
	float                              x = 0.0f;
	float                              y = 0.0f;
	std::vector<RenderGraphPortRecord> outputs;

	bool operator==(const RenderGraphNodeRecord&) const = default;

	/**
	 * @brief Serializes the node.
	 * @tparam Archive Cereal archive type.
	 * @param ar Archive to read or write.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("name", name),
		   cereal::make_nvp("pass", pass));
		if constexpr (Archive::is_loading::value)
		{
			shaderUid.reset();
			int uid = 0;
			if (ReadOptionalGraphField(ar, "shader", uid))
			{
				if (uid < 0)
					throw cereal::Exception("Invalid graph shader UID");
				shaderUid = uid;
			}
		}
		else if (shaderUid)
			ar(cereal::make_nvp("shader", *shaderUid));
		ar(cereal::make_nvp("x", x),
		   cereal::make_nvp("y", y),
		   cereal::make_nvp("outputs", outputs));
	}
};

/**
 * @brief One saved connection: producer port to consumer port, by node name.
 */
struct RenderGraphEdgeRecord
{
	std::string from;
	std::string fromPort;
	std::string to;
	std::string toPort;

	// The input role survives a missing custom shader even when the saved name was edited.
	std::string inputType;
	std::uint32_t inputBinding = 0;

	bool operator==(const RenderGraphEdgeRecord&) const = default;

	/**
	 * @brief Serializes the edge endpoints.
	 * @tparam Archive Cereal archive type.
	 * @param ar Archive to read or write.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("from", from),
		   cereal::make_nvp("fromPort", fromPort),
		   cereal::make_nvp("to", to),
		   cereal::make_nvp("toPort", toPort));
		if constexpr (Archive::is_loading::value)
		{
			if (ReadOptionalGraphField(ar, "inputType", inputType))
				ar(cereal::make_nvp("inputBinding", inputBinding));
		}
		else if (!inputType.empty())
			ar(cereal::make_nvp("inputType", inputType), cereal::make_nvp("inputBinding", inputBinding));
	}
};

/**
 * @brief The whole document in its saved form.
 */
struct SceneOutputRecord
{
	std::string name;
	std::string node;
	std::string port;
	bool operator==(const SceneOutputRecord&) const = default;

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(name), CEREAL_NVP(node), CEREAL_NVP(port));
	}
};

struct RenderGraphRecord
{
	std::vector<RenderGraphNodeRecord> nodes;
	std::vector<RenderGraphEdgeRecord> edges;
	std::vector<SceneOutputRecord> outputs;

	bool operator==(const RenderGraphRecord&) const = default;

	/**
	 * @brief Serializes the node and edge lists.
	 * @tparam Archive Cereal archive type.
	 * @param ar Archive to read or write.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("nodes", nodes),
		   cereal::make_nvp("edges", edges),
		   cereal::make_nvp("outputs", outputs));
	}
};

/// Supplies the pass catalog to graph records loaded from this project.
std::shared_ptr<const ResourceReadServices> MakeGraphReadServices(
	const IPassShaderCatalog& catalog, std::weak_ptr<std::vector<std::string>> diagnostics = {});

/// Rebuilds a freshly deserialized graph once its references can resolve.
bool RestoreGraph(RenderGraphData& graph, const RenderGraphRecord& record,
                  const IResourceLookup& pool, const ResourceReadServices* services);

/// Captures one node, including its shader UID, position and tagged outputs.
RenderGraphNodeRecord ToRecord(const Node<CpuPort, CpuNode>& node);

/// Rebuilds the node's sockets from the recorded outputs and its pass input roles.
std::unique_ptr<Node<CpuPort, CpuNode>> FromRecord(
	const RenderGraphNodeRecord& saved, const IPortSource& ports, std::vector<std::string>& problems,
	const IResourceLookup* resources,
	const std::function<bool(const std::string&, const ShaderData&)>& acceptsShader = {});

/**
 * @brief Flattens a live document into the project's record.
 *
 * Node fields and output sockets with their tags are written; input sockets are
 * not, because a load derives them from the node's shader.
 *
 * @param graph  Document to read; it is not modified.
 */
RenderGraphRecord ToRecord(const RenderGraphData& graph);

/**
 * @brief Rebuilds a live document from a saved record.
 *
 * Nodes and output sockets come from the record; input sockets are derived from
 * each node's pass kind through @p ports. Every refused connection, unknown node
 * or missing port appends a message to @p problems instead of throwing, so a
 * partly broken file still opens and validation can report the rest.
 *
 * @param record    Saved document.
 * @param ports     Pass-kind port source.
 * @param problems  Receives one message per recoverable inconsistency.
 */
std::unique_ptr<RenderGraphData> FromRecord(
	const RenderGraphRecord& record, const IPortSource& ports, std::vector<std::string>& problems,
	const IResourceLookup* resources = nullptr);

} // namespace neurus::project
