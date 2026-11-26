#pragma once

#include <cereal/archives/json.hpp>

#include <sstream>
#include <string>

namespace neurus::project
{

class Serializable
{
public:
    virtual ~Serializable() = default;
    virtual const char* Key() const noexcept = 0;
    virtual void Save(cereal::JSONOutputArchive& ar) const = 0;
    /** @brief Loads an isolated component object so a failure cannot move another component's cursor. */
    virtual void LoadJson(const std::string& object) = 0;
};

template<class Component>
void LoadComponentJson(const std::string& object, Component& component)
{
    std::istringstream stream(object);
    cereal::JSONInputArchive archive(stream);
    component.Load(archive);
}

} // namespace neurus::project
