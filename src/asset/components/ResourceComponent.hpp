/**
 * @file ResourceComponent.hpp
 * @brief Serializes a ResourceManager's UID pool into a project file.
 *
 * Opening a project registers each pooled UID with its name, concrete type
 * and serialized record. A lookup restores the CPU object and its dependencies.
 * An unusable optional shader selects the existing built-in fallback.
 */

#pragma once

#include "asset/Serializable.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace neurus { class ResourceManager; class IPassShaderCatalog; }

namespace neurus::project
{

/**
 * @brief Persists a ResourceManager's UID pool in a project file.
 */
class ResourceComponent : public Serializable
{
public:
	ResourceComponent(ResourceManager& resources, const IPassShaderCatalog& catalog,
	                  std::vector<std::string>* diagnostics = nullptr);

	const char* Key() const noexcept override { return "m_resources"; }
	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;
	void LoadJson(const std::string& object) override;

private:
	ResourceManager* m_resources;
	const IPassShaderCatalog& m_catalog;
	// Deferred readers use a weak handle so later lookups only append to a
	// borrowed diagnostics list while its project component is alive.
	std::shared_ptr<std::reference_wrapper<std::vector<std::string>>> m_diagnostics;
};

} // namespace neurus::project
