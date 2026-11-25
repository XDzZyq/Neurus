#pragma once

#include <memory>
#include <string_view>

namespace neurus {

class IPortSource;
class ShaderData;

/// CPU pass definitions used to validate and prepare a graph for rendering.
class IPassShaderCatalog
{
public:
	virtual ~IPassShaderCatalog() = default;
	virtual const IPortSource& Ports() const = 0;
	virtual std::shared_ptr<const ShaderData> ShaderFor(std::string_view name) const = 0;
};

} // namespace neurus
