#pragma once

#include <memory>
#include <string_view>

namespace neurus {

class IPortSource;
class ShaderData;

/// CPU pass definitions needed to restore a graph from a project.
class IPassShaderCatalog
{
public:
	virtual ~IPassShaderCatalog() = default;
	/// Retains the same catalog for readers that outlive the project components.
	virtual std::shared_ptr<const IPassShaderCatalog> Share() const = 0;
	virtual const IPortSource& Ports() const = 0;
	virtual std::shared_ptr<const ShaderData> ShaderFor(std::string_view name) const = 0;
};

} // namespace neurus
