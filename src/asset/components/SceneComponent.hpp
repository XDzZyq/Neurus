#pragma once

#include "asset/Serializable.hpp"

namespace neurus { class ResourceManager; class Scene; }

namespace neurus::project
{

class SceneComponent : public Serializable
{
public:
	explicit SceneComponent(Scene& scene, ResourceManager& resources);
	const char* Key() const noexcept override { return "m_scene"; }
	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;
private:
	Scene* m_scene;
	ResourceManager* m_resources;
};

} // namespace neurus::project
