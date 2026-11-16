#pragma once

#include "asset/Serializable.hpp"

namespace neurus { class RenderConfig; }

namespace neurus::project
{

class ConfigComponent : public Serializable
{
public:
    explicit ConfigComponent(RenderConfig& config);
    const char* Key() const noexcept override { return "proj_config"; }
    void Save(cereal::JSONOutputArchive& ar) const override;
    void Load(cereal::JSONInputArchive& ar) override;
private:
    RenderConfig* m_config;
};

} // namespace neurus::project
