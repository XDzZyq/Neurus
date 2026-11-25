/** @file RenderGraphRecord.hpp
 *  @brief Portable node and edge records for a pooled graph.
 */
#pragma once

#include "asset/data/render_graph/CpuPorts.hpp"
#include "asset/ResourceRef.hpp"
#include "asset/data/ShaderData.hpp"
#include "core/Serialize.hpp"

#include <cereal/archives/json.hpp>
#include <cereal/types/optional.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neurus
{
using PassShaderRef = ResourceRef<const ShaderData>;
class IResourceLookup;
class RenderGraphData;
template<class, class> class Node;
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
/// Stores a socket's metadata. Its direction comes from the input or output list.
template<class Archive>
void serialize(Archive& ar, CpuPort& port)
{
	ar(cereal::make_nvp("name", port.name),
	   cereal::make_nvp("type", port.type),
	   cereal::make_nvp("binding", port.binding),
	   cereal::make_nvp("location", port.location),
	   cereal::make_nvp("tag", port.tag));
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
 * @brief One saved node: its document id, pass, shader, layout and sockets.
 */
struct RenderGraphNodeRecord
{
	std::string                        name;
	std::string                        pass;
	std::optional<int>                 shaderUid;
	float                              x = 0.0f;
	float                              y = 0.0f;
	std::vector<CpuPort>               inputs;
	std::vector<CpuPort>               outputs;

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
		   cereal::make_nvp("inputs", inputs),
		   cereal::make_nvp("outputs", outputs));
		if constexpr (Archive::is_loading::value)
			for (auto& port : outputs)
				port.direction = PortDirection::Output;
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

/// Rebuilds the saved nodes and edges and reports shader mismatches.
bool RestoreGraph(RenderGraphData& graph, const RenderGraphRecord& record,
                  const IResourceLookup& pool);

/// Captures one node, including its shader UID, position and sockets.
RenderGraphNodeRecord ToRecord(const Node<CpuPort, CpuNode>& node);

/// Rebuilds one node from its saved sockets and resolves its shader through the pool.
std::unique_ptr<Node<CpuPort, CpuNode>> FromRecord(
	const RenderGraphNodeRecord& saved, const IResourceLookup& pool);

/// Flattens the saved topology, including both input and output sockets.
RenderGraphRecord ToRecord(const RenderGraphData& graph);

/// Rebuilds saved sockets and edges. Broken connections are reported in problems.
std::unique_ptr<RenderGraphData> FromRecord(
	const RenderGraphRecord& record, std::vector<std::string>& problems,
	const IResourceLookup* pool = nullptr);

} // namespace neurus::project
