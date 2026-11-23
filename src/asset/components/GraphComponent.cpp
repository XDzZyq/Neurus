#include "asset/components/GraphComponent.hpp"

#include "asset/data/RenderGraphData.hpp"
#include "asset/components/GraphRecordJson.hpp"
#include "core/Log.hpp"
#include "core/ResourceManager.hpp"
#include "core/Serialize.hpp"

#include <cereal/cereal.hpp>

#include <exception>
#include <limits>
#include <optional>

#include <nlohmann/json.hpp>
#include <utility>
#include <variant>

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
	std::variant<int, RenderGraphRecord> selection;

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
	if (const auto outputs = graph.find("outputs"); outputs != graph.end())
	{
		if (!outputs->is_array())
			return false;
		for (const auto& slot : *outputs)
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

GraphComponent::GraphComponent(std::shared_ptr<RenderGraphData>& graph, ResourceManager& resources,
                               const IPassShaderCatalog& catalog, std::vector<std::string>& diagnostics)
	: m_graph(graph), m_resources(resources), m_catalog(catalog), m_diagnostics(diagnostics)
{
}

void GraphComponent::Save(cereal::JSONOutputArchive& ar) const
{
	if (!m_graph)
		return;
	SavedSelection active{m_graph->GetObjectID()};
	ar(cereal::make_nvp("proj_render_graph", active));
}

void GraphComponent::LoadJson(const std::string& object)
{
	m_graph.reset();
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
		NEURUS_LOG("[GraphComponent] cannot load graph: " << error.what() << " - keeping null");
	}
}

void GraphComponent::Load(cereal::JSONInputArchive& ar)
{
	// Every field is optional at the archive level; an absent key means "no
	// saved document", never the previous session's graph.
	m_graph.reset();
	SavedSelection loaded;
	try
	{
		if (!archive::LoadFresh(ar, "proj_render_graph", loaded))
		{
			NEURUS_LOG("GraphComponent::Load: no proj_render_graph node"
			           " - using runtime defaults without storing a project graph.");
			return;
		}
	}
	catch (const cereal::Exception& e)
	{
		NEURUS_ERR("GraphComponent::Load: " << e.what()
		           << " - using runtime defaults (corrupt graph).");
		return;
	}

	if (const int* uid = std::get_if<int>(&loaded.selection))
	{
		m_graph = m_resources.Get<RenderGraphData>(*uid);
		if (!m_graph)
		{
			m_diagnostics.push_back("Missing render graph UID " + std::to_string(*uid));
			NEURUS_LOG("[GraphComponent] " << m_diagnostics.back());
		}
		return;
	}

	// Older projects store the selected graph inline. Adopt it only after resolution succeeds.
	auto graph = std::make_shared<RenderGraphData>(std::move(std::get<RenderGraphRecord>(loaded.selection)));
	std::vector<std::string> diagnostics;
	if (graph->Resolve(m_catalog, m_resources, diagnostics))
	{
		m_resources.Register(graph);
		m_graph = std::move(graph);
	}
	for (const auto& diagnostic : diagnostics)
		NEURUS_LOG("[GraphComponent] " << diagnostic);
	m_diagnostics.insert(m_diagnostics.end(), diagnostics.begin(), diagnostics.end());
}

} // namespace neurus::project
