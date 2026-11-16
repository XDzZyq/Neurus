/**
 * @file GraphComponent.hpp
 * @brief Project component that stores the scene render graph document.
 */

#pragma once

#include "asset/Serializable.hpp"

namespace neurus { struct RenderGraphSpec; }

namespace neurus::project
{

/**
 * @brief Saves and loads the render graph document under `proj_render_graph`.
 *
 * A project written before the document existed loads the built-in default.
 */
class GraphComponent : public Serializable
{
public:
	explicit GraphComponent(RenderGraphSpec& spec);

	const char* Key() const noexcept override { return "proj_render_graph"; }
	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;

private:
	RenderGraphSpec* m_spec;
};

} // namespace neurus::project
