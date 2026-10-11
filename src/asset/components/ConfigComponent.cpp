#include "asset/components/ConfigComponent.h"
#include "core/RenderConfig.h"
#include "core/Log.h"
#include "core/Serialize.h"

namespace neurus::project
{

ConfigComponent::ConfigComponent(RenderConfig& config) : m_config(&config) {}

void ConfigComponent::Save(cereal::JSONOutputArchive& ar) const
{
    ar(cereal::make_nvp("proj_config", *m_config));
}

void ConfigComponent::Load(cereal::JSONInputArchive& ar)
{
    // Read into a default-constructed RenderConfig: every field is optional,
    // so a key the file lacks resolves to the in-class default rather than to
    // the previous session's value, and a malformed file cannot half-apply.
    try
    {
        if (!archive::LoadFresh(ar, "proj_config", *m_config))
            NEURUS_ERR("ConfigComponent::Load: no proj_config node"
                " - defaulted (old project format).");
    }
    catch (const cereal::Exception& e)
    {
        // Still reachable: a present-but-malformed node throws from inside
        // itself, past LoadFresh's per-field catch.
        NEURUS_ERR("ConfigComponent::Load: " << e.what()
            << " - proj_config defaulted (corrupt node).");
        *m_config = RenderConfig{};
    }
}

} // namespace neurus::project
