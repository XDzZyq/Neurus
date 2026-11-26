/**
 * @file BuiltInRenderData.hpp
 * @brief Built-in scene shaders, pass ports and default render graph, held in a private pool.
 */

#pragma once

#include "asset/data/render_graph/PassShaderCatalog.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace neurus {

class ShaderData;
class RenderGraphData;

/**
 * @brief Application-scoped CPU catalog of default scene shaders, ports and graph.
 *
 * First use prepares the catalog once. Prepare it on the owning thread before
 * starting the renderer, since creating CPU shader data allocates UIDs.
 */
class BuiltInRenderData : public IPassShaderCatalog
{
public:
	BuiltInRenderData();
	~BuiltInRenderData() override;
	/// Copies share the lazily prepared shader and port data.
	BuiltInRenderData(const BuiltInRenderData&) = default;
	BuiltInRenderData& operator=(const BuiltInRenderData&) = delete;

	const std::shared_ptr<const RenderGraphData>& Graph() const;
	const IPortSource& Ports() const override;
	/** @brief Returns the compiled default shader for a recipe name, or null if unknown. */
	std::shared_ptr<const ShaderData> ShaderFor(std::string_view name) const override;

	/// Makes an independently editable copy of the pass's primary shader with a fresh UID.
	std::shared_ptr<ShaderData> CreatePassShader(std::string_view passKind, const std::string& name) const;

private:
	struct Impl;
	const Impl& Data() const;

	struct SharedState;
	std::shared_ptr<SharedState> m_state;
};

} // namespace neurus
