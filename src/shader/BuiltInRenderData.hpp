#pragma once

#include <memory>
#include <string_view>

namespace neurus {

class ComputeShader;
class RenderShader;
class RenderGraphData;
class IPortSource;

/** Immutable CPU defaults, cached independently of every project's resource pool. */
class BuiltInRenderData
{
public:
	static const BuiltInRenderData& Get();
	~BuiltInRenderData();
	BuiltInRenderData(const BuiltInRenderData&) = delete;
	BuiltInRenderData& operator=(const BuiltInRenderData&) = delete;

	const std::shared_ptr<const RenderGraphData>& Graph() const;
	const IPortSource& Ports() const;
	std::shared_ptr<const RenderShader> RenderShaderFor(std::string_view name) const;
	std::shared_ptr<const ComputeShader> ComputeShaderFor(std::string_view name) const;

private:
	BuiltInRenderData();
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace neurus
