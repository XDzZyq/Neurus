/** @file GraphComponent.hpp
 *  @brief Saves the active graph UID and accepts inline graphs from older projects.
 */
#pragma once

#include "asset/Serializable.hpp"
#include "asset/data/render_graph/RenderGraphRecord.hpp"

#include <memory>
#include <optional>
#include <variant>

namespace neurus
{
class RenderGraphData;
}

namespace neurus::project
{

using GraphSelection = std::variant<int, RenderGraphRecord>;

class GraphComponent : public Serializable
{
public:
	/**
	 * @brief Binds the component to the Editor's document slots.
	 * @param pending  Receives the graph UID or legacy record read from the file.
	 * @param graph    Nullable project document, read on save; null omits the key.
	 */
	GraphComponent(std::optional<GraphSelection>& pending,
	               const std::shared_ptr<RenderGraphData>& graph);

	const char* Key() const noexcept override { return "proj_render_graph"; }
	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;
	void LoadJson(const std::string& object) override;

private:
	std::optional<GraphSelection>* m_pending;
	const std::shared_ptr<RenderGraphData>* m_graph;
};

} // namespace neurus::project
