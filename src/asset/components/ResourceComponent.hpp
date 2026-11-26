/**
 * @file ResourceComponent.hpp
 * @brief Serializes a ResourceManager's UID pool into a project file.
 *
 * Opening a project registers each pooled UID with its name, concrete type
 * and serialized record. A lookup restores the CPU object and its dependencies.
 */

#pragma once

#include "asset/Serializable.hpp"

#include <string>

namespace neurus { class ResourceManager; }

namespace neurus::project
{

/**
 * @brief Persists a ResourceManager's UID pool in a project file.
 */
class ResourceComponent : public Serializable
{
public:
	explicit ResourceComponent(ResourceManager& resources);

	const char* Key() const noexcept override { return "m_resources"; }
	void Save(cereal::JSONOutputArchive& ar) const override;
	void LoadJson(const std::string& object) override;

private:
	ResourceManager& m_resources;
};

} // namespace neurus::project
