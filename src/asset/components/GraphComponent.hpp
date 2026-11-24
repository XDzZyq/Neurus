/** @file GraphComponent.hpp
 *  @brief Persists the selected pooled render graph.
 */
#pragma once

#include "asset/Serializable.hpp"
#include "asset/ResourceRef.hpp"

#include <memory>
#include <string>
#include <vector>

namespace neurus
{
class RenderGraphData;
class ResourceManager;
}

namespace neurus::project
{

class GraphComponent : public Serializable
{
public:
	/// Loads the active selection against the already restored resource pool.
	GraphComponent(ResourceRef<RenderGraphData>& graph, ResourceManager& resources,
	               std::vector<std::string>& diagnostics);

	const char* Key() const noexcept override { return "proj_render_graph"; }
	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;
	void LoadJson(const std::string& object) override;

private:
	ResourceRef<RenderGraphData>& m_graph;
	ResourceManager& m_resources;
	std::vector<std::string>& m_diagnostics;
};

} // namespace neurus::project
