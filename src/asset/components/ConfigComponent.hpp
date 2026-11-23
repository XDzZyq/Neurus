#pragma once

#include "asset/Serializable.hpp"
#include <memory>

namespace neurus { class RenderConfig; }

namespace neurus::project
{

class ConfigComponent : public Serializable
{
public:
    explicit ConfigComponent(std::shared_ptr<RenderConfig>& config);
    const char* Key() const noexcept override { return "proj_config"; }
    void Save(cereal::JSONOutputArchive& ar) const override;
    void Load(cereal::JSONInputArchive& ar) override;
    void LoadJson(const std::string& object) override;
private:
    std::shared_ptr<RenderConfig>* m_config;
};

} // namespace neurus::project
