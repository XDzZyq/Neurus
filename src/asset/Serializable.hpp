#pragma once

#include <cereal/archives/json.hpp>

namespace neurus::project
{

class Serializable
{
public:
    virtual ~Serializable() = default;
    virtual const char* Key() const noexcept = 0;
    virtual void Save(cereal::JSONOutputArchive& ar) const = 0;
    virtual void Load(cereal::JSONInputArchive& ar) = 0;
};

} // namespace neurus::project
