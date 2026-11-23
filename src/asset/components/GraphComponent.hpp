/** @file GraphComponent.hpp
 *  @brief Saves the active graph UID and accepts inline graphs from older projects.
 */
#pragma once

#include "asset/Serializable.hpp"

#include <memory>
#include <string>
#include <vector>

namespace neurus
{
class RenderGraphData;
class IPassShaderCatalog;
class ResourceManager;
}

namespace neurus::project
{

class GraphComponent : public Serializable
{
public:
	/// Loads the active selection against the already restored resource pool.
	GraphComponent(std::shared_ptr<RenderGraphData>& graph, ResourceManager& resources,
	               const IPassShaderCatalog& catalog, std::vector<std::string>& diagnostics);

	const char* Key() const noexcept override { return "proj_render_graph"; }
	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;
	void LoadJson(const std::string& object) override;

private:
	std::shared_ptr<RenderGraphData>& m_graph;
	ResourceManager& m_resources;
	const IPassShaderCatalog& m_catalog;
	std::vector<std::string>& m_diagnostics;
};

} // namespace neurus::project
