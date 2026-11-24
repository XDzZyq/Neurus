#include "asset/components/GraphRecordJson.hpp"

#include "asset/data/render_graph/GraphOutputTag.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>

namespace neurus::project
{
namespace
{
using Json = nlohmann::ordered_json;

bool UnsignedWithin(const Json& value, const uint64_t maximum)
{
	if (value.is_number_unsigned())
		return value.get<uint64_t>() <= maximum;
	return value.is_number_integer() && value.get<int64_t>() >= 0
	    && static_cast<uint64_t>(value.get<int64_t>()) <= maximum;
}

bool HasString(const Json& object, const char* key)
{
	const auto value = object.find(key);
	return value != object.end() && value->is_string();
}

bool ValidTag(const Json& tag)
{
	if (!tag.is_object() || !tag.contains("nullopt") || !tag["nullopt"].is_boolean())
		return false;
	if (tag["nullopt"].get<bool>())
		return true;
	if (!tag.contains("data") || !tag["data"].is_object())
		return false;
	const auto& data = tag["data"];
	return data.contains("format") && UnsignedWithin(data["format"], static_cast<uint64_t>(GraphFormat::D32Sfloat))
	    && data.contains("usage") && UnsignedWithin(data["usage"], static_cast<uint64_t>(GraphUsage::DepthSampled))
	    && data.contains("extent") && UnsignedWithin(data["extent"], static_cast<uint64_t>(GraphExtent::Shadow))
	    && data.contains("layers") && UnsignedWithin(data["layers"], static_cast<uint64_t>(GraphLayers::Lights));
}

} // namespace

bool ValidGraphShape(const Json& graph)
{
	if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()
		|| !graph.contains("edges") || !graph["edges"].is_array())
		return false;
	for (const auto& node : graph["nodes"])
	{
		if (!node.is_object() || !HasString(node, "name") || !HasString(node, "pass")
			|| !node.contains("x") || !node["x"].is_number()
			|| !node.contains("y") || !node["y"].is_number()
			|| !node.contains("outputs") || !node["outputs"].is_array())
			return false;
		if (node.contains("shader") && !UnsignedWithin(node["shader"], std::numeric_limits<int>::max()))
			return false;
		for (const auto& port : node["outputs"])
		{
			if (!port.is_object() || !HasString(port, "name") || !HasString(port, "type")
				|| !port.contains("binding") || !UnsignedWithin(port["binding"], std::numeric_limits<uint32_t>::max())
				|| !port.contains("location") || !UnsignedWithin(port["location"], std::numeric_limits<uint32_t>::max())
				|| !port.contains("tag") || !ValidTag(port["tag"]))
				return false;
		}
	}
	const auto outputs = graph.find("outputs");
	if (outputs == graph.end() || !outputs->is_array())
		return false;
	for (const auto& slot : *outputs)
	{
		if (!slot.is_object() || !HasString(slot, "name") || !HasString(slot, "node") || !HasString(slot, "port"))
			return false;
	}
	for (const auto& edge : graph["edges"])
	{
		if (!edge.is_object() || !HasString(edge, "from") || !HasString(edge, "fromPort")
			|| !HasString(edge, "to") || !HasString(edge, "toPort"))
			return false;
		if (edge.contains("inputType") && (!HasString(edge, "inputType") || !edge.contains("inputBinding")
		    || !UnsignedWithin(edge["inputBinding"], std::numeric_limits<uint32_t>::max())))
			return false;
	}
	return true;
}

} // namespace neurus::project
