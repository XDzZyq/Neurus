#pragma once

#include <memory>
#include <mutex>
#include <string_view>

namespace neurus {

class ShaderData;
class RenderGraphData;
class IPortSource;

/**
 * @brief Application-scoped CPU catalog of default scene shaders, ports and graph.
 *
 * First use prepares the catalog once. Prepare it on the owning thread before
 * starting the renderer, since creating CPU shader data allocates UIDs.
 */
class BuiltInRenderData
{
public:
	BuiltInRenderData();
	~BuiltInRenderData();
	BuiltInRenderData(const BuiltInRenderData&) = delete;
	BuiltInRenderData& operator=(const BuiltInRenderData&) = delete;

	const std::shared_ptr<const RenderGraphData>& Graph() const;
	const IPortSource& Ports() const;
	/** @brief Returns the compiled default shader for a recipe name, or null if unknown. */
	std::shared_ptr<const ShaderData> ShaderFor(std::string_view name) const;

private:
	struct Impl;
	const Impl& Data() const;

	mutable std::once_flag m_initialize;
	mutable std::unique_ptr<const Impl> m_impl;
};

} // namespace neurus
