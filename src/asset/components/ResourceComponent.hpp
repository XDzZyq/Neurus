/**
 * @file ResourceComponent.hpp
 * @brief Serializes a ResourceManager's UID pool into a project file.
 *
 * Save writes the whole pool polymorphically; Load restores it and wires the
 * per-object data references (mesh -> MeshData/Shader, environment ->
 * ImageData) so the pool is uploadable after a reload. Failed shaders are
 * removed on load and their mesh references become null; runtime built-ins are
 * supplied separately and are never inserted into the serialized pool.
 */

#pragma once

#include "asset/Serializable.hpp"

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
	void LoadJson(const std::string& object) override;

private:
	void RestoreReferences();

	ResourceManager* m_resources;
};

} // namespace neurus::project
