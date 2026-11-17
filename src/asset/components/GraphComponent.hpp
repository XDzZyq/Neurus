/**
 * @file GraphComponent.hpp
 * @brief Project component that stores the scene render graph document.
 */

#pragma once

#include "asset/Serializable.hpp"
#include "asset/graph/GraphDocument.hpp"

#include <optional>

namespace neurus::project
{

/**
 * @brief Saves and loads the render graph document under `proj_render_graph`.
 *
 * The live document cannot be serialized directly (nodes own their sockets and
 * edges are pointers), so the component reads a flat record and the Editor
 * applies it once the pool is restored. A project written before the document
 * existed, or a corrupt node, leaves no pending record and the Editor builds
 * the built-in default.
 */
class GraphComponent : public Serializable
{
public:
	/**
	 * @brief Binds the component to the Editor's document slots.
	 * @param pending  Receives the record read from the file.
	 * @param graph    Live document, read on save.
	 */
	GraphComponent(std::optional<CpuGraphRecord>& pending, const CpuRenderGraph& graph);

	const char* Key() const noexcept override { return "proj_render_graph"; }
	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;

private:
	std::optional<CpuGraphRecord>* m_pending;
	const CpuRenderGraph*          m_graph;
};

} // namespace neurus::project
