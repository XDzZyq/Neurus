#pragma once

#include <string_view>

namespace neurus {

class IPortSource;
class ShaderData;

/// CPU pass definitions needed to restore a graph from a project.
class IPassShaderCatalog
{
public:
	virtual ~IPassShaderCatalog() = default;
	virtual const IPortSource& Ports() const = 0;
	virtual void ValidatePassShader(std::string_view kind, const ShaderData& shader) const = 0;
};

} // namespace neurus
