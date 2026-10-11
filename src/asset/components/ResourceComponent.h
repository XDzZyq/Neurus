/**
 * @file ResourceComponent.h
 * @brief Serializes a ResourceManager's UID pool into a project file.
 *
 * Save writes the whole pool polymorphically; Load restores it and wires the
 * per-object data references (mesh -> MeshData/Shader, environment ->
 * ImageData) so the pool is uploadable after a reload.
 */

#pragma once

#include "asset/Serializable.h"

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
	void Load(cereal::JSONInputArchive& ar) override;

private:
	ResourceManager* m_resources;
};

} // namespace neurus::project
