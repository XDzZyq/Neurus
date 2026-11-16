#include "asset/components/UIComponent.hpp"
#include "core/Log.hpp"

namespace neurus::project
{

UIComponent::UIComponent(std::string& ui) : m_ui(&ui) {}

void UIComponent::Save(cereal::JSONOutputArchive& ar) const
{
    ar(cereal::make_nvp("ui", *m_ui));
}

void UIComponent::Load(cereal::JSONInputArchive& ar)
{
    try { ar(cereal::make_nvp("ui", *m_ui)); }
    catch (const cereal::Exception& e)
    {
        NEURUS_ERR("UIComponent::Load: " << e.what()
            << " - ui state defaulted (old project format).");
        m_ui->clear();
    }
}

} // namespace neurus::project
