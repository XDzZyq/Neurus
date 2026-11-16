/**
 * @file GlmSerialization.hpp
 * @brief Custom cereal serialization functions for glm math types.
 *
 * Cereal does not natively support glm::vec3 / glm::vec4.
 * These free-function serialize overloads in the cereal namespace
 * enable NVP-based serialization of glm vector types.
 */
#pragma once

#include <cereal/cereal.hpp>
#include <glm/glm.hpp>

namespace cereal
{

template<class Archive>
void serialize(Archive& ar, glm::vec3& v)
{
	ar(make_nvp("x", v.x), make_nvp("y", v.y), make_nvp("z", v.z));
}

template<class Archive>
void serialize(Archive& ar, glm::vec4& v)
{
	ar(make_nvp("x", v.x), make_nvp("y", v.y), make_nvp("z", v.z), make_nvp("w", v.w));
}

} // namespace cereal
