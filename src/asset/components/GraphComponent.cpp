#include "asset/components/GraphComponent.hpp"

#include "asset/data/RenderGraphData.hpp"
#include "asset/components/GraphRecordJson.hpp"
#include "core/Log.hpp"
#include "core/Serialize.hpp"

#include <cereal/cereal.hpp>

#include <exception>
#include <limits>
#include <optional>

#include <nlohmann/json.hpp>
#include <utility>

namespace neurus::project
{

namespace {

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

// This adapter preserves the flat legacy shape while saving only an active UID.
struct SavedSelection
{
	GraphSelection selection;

	template<class Archive>
	void serialize(Archive& ar)
	{
		if constexpr (Archive::is_saving::value)
		{
			ar(cereal::make_nvp("uid", std::get<int>(selection)));
		}
		else
		{
			int uid = 0;
			if (archive::OptionalField(ar, cereal::make_nvp("uid", uid)))
				selection = uid;
			else
				selection.emplace<RenderGraphRecord>().serialize(ar);
		}
	}
};

} // anonymous namespace

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
	for (const auto& edge : graph["edges"])
	{
		if (!edge.is_object() || !HasString(edge, "from") || !HasString(edge, "fromPort")
			|| !HasString(edge, "to") || !HasString(edge, "toPort"))
			return false;
	}
	return true;
}

GraphComponent::GraphComponent(std::optional<GraphSelection>& pending,
                               const std::shared_ptr<RenderGraphData>& graph)
	: m_pending(&pending)
	, m_graph(&graph)
{
}

void GraphComponent::Save(cereal::JSONOutputArchive& ar) const
{
	if (!*m_graph)
		return;
	SavedSelection active{(*m_graph)->GetObjectID()};
	ar(cereal::make_nvp("proj_render_graph", active));
}

void GraphComponent::LoadJson(const std::string& object)
{
	m_pending->reset();
	try
	{
		const Json document = Json::parse(object);
		const auto graph = document.find("proj_render_graph");
		if (graph == document.end() || graph->is_null())
			return;
		const auto uid = graph->find("uid");
		const bool valid = uid != graph->end()
		    ? UnsignedWithin(*uid, std::numeric_limits<int>::max() - 1)
		    : ValidGraphShape(*graph);
		if (!valid)
		{
			NEURUS_LOG("[GraphComponent] malformed graph - keeping null and using runtime defaults");
			return;
		}
		// Guard all scalar/container types before cereal's unchecked Release reads.
		Serializable::LoadJson(object);
	}
	catch (const std::exception& error)
	{
		m_pending->reset();
		NEURUS_LOG("[GraphComponent] cannot load graph: " << error.what() << " - keeping null");
	}
}

void GraphComponent::Load(cereal::JSONInputArchive& ar)
{
	// Every field is optional at the archive level; an absent key means "no
	// saved document", never the previous session's graph.
	m_pending->reset();
	SavedSelection loaded;
	try
	{
		if (!archive::LoadFresh(ar, "proj_render_graph", loaded))
		{
			NEURUS_LOG("GraphComponent::Load: no proj_render_graph node"
			           " - using runtime defaults without storing a project graph.");
			m_pending->reset();
			return;
		}
	}
	catch (const cereal::Exception& e)
	{
		NEURUS_ERR("GraphComponent::Load: " << e.what()
		           << " - using runtime defaults (corrupt graph).");
		m_pending->reset();
		return;
	}

	*m_pending = std::move(loaded.selection);
}

} // namespace neurus::project
